// SPDX-License-Identifier: Apache-2.0

#include "partid_rpc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "partid_camera.h"
#include "partid_portal.h"
#include "partid_ui.h"
#include "partid_config.h"
#include "partid_net.h"

static const char *TAG = "partid_rpc";

/*
 * Single owner. Every handler below runs on the one ESP-Iris service task, so
 * the handlers need no locking against each other. The published snapshot is
 * the exception: it is written by the query worker and read by a handler, so
 * it takes a mutex.
 */
static struct {
    partid_rpc_trigger_fn trigger;
    SemaphoreHandle_t lock;
    bool have_snapshot;
    uint32_t seq;
    esp_err_t status;
    partid_vision_result_t result;
    uint32_t connect_ms;
    uint32_t encode_us;
    size_t jpeg_bytes;
    int8_t rssi;

    /* Handler scratch. CONFIG_ESP_IRIS_SERVICE_STACK_SIZE is 4096 and these
     * aggregates are most of a kilobyte between them, so they live with the
     * module rather than on that task's stack. Safe because the handlers share
     * one task and never run concurrently. */
    char line[PARTID_PROMPT_CAP + 16];
    char described[224];
} s;

/*
 * Always ESP_OK, and the outcome goes in the text.
 *
 * ESP-Iris treats a non-ESP_OK handler return as a transport failure: the
 * Gateway answers HTTP 409 device_request_failed, the CLI prints
 * "device_unavailable", and the response bytes are thrown away. So returning
 * ESP_ERR_NOT_FOUND for an unknown key loses the message that said which key,
 * and reports a perfectly healthy board as missing. Every handler here
 * answered the question it was asked, which is what ESP_OK means at this
 * layer, so refusals are carried as words instead.
 */
static esp_err_t reply(uint8_t *response, size_t capacity, size_t *size,
                       const char *fmt, ...)
{
    if (!response || !size || !capacity) {
        return ESP_OK;
    }
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf((char *)response, capacity, fmt, args);
    va_end(args);
    /* vsnprintf reports what it wanted, not what it wrote. */
    *size = n < 0 ? 0 : ((size_t)n >= capacity ? capacity - 1 : (size_t)n);
    return ESP_OK;
}

static esp_err_t handle_set(const esp_iris_rpc_request_t *request, uint8_t *response,
                            size_t capacity, size_t *size, void *user_ctx)
{
    (void)user_ctx;

    /* Bounded by the field table, so a long prompt is the worst case. */
    if (!request->payload || request->payload_size == 0 ||
        request->payload_size >= sizeof(s.line)) {
        return reply(response, capacity, size,
                     "payload must be 1 to %u bytes of key=value",
                     (unsigned)sizeof(s.line) - 1U);
    }
    memcpy(s.line, request->payload, request->payload_size);
    s.line[request->payload_size] = '\0';

    char *split = strchr(s.line, '=');
    if (!split) {
        return reply(response, capacity, size,
                     "expected key=value, keys ssid pass gkey model prompt cycle");
    }
    *split = '\0';

    const esp_err_t err = partid_config_set(s.line, split + 1);
    if (err == ESP_OK) {
        /* Rotation is applied live. The whole reason it is a setting is to
         * compare angles against what the model reads without a rebuild
         * between each one. */
        if (strcmp(s.line, "rot") == 0) {
            partid_camera_set_rotation((uint16_t)atoi(split + 1));
        }
        return reply(response, capacity, size, "ok %s", s.line);
    }
    return reply(response, capacity, size, "failed %s, 0x%x", s.line, (int)err);
}

static esp_err_t handle_status(const esp_iris_rpc_request_t *request, uint8_t *response,
                               size_t capacity, size_t *size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    /* partid_config_t is over 600 bytes, so it is loaded into the module's
     * own storage for the same reason as the scratch above. */
    static partid_config_t cfg;
    const esp_err_t err = partid_config_load(&cfg);
    if (err != ESP_OK) {
        return reply(response, capacity, size, "config unreadable 0x%x", (int)err);
    }

    partid_config_describe(&cfg, s.described, sizeof(s.described));
    return reply(response, capacity, size, "%s ready=%d wifi=%s rssi=%d",
                 s.described, partid_config_complete(&cfg) ? 1 : 0,
                 partid_net_is_up() ? "up" : "down", (int)partid_net_rssi());
}

static esp_err_t handle_query(const esp_iris_rpc_request_t *request, uint8_t *response,
                              size_t capacity, size_t *size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    if (!s.trigger) {
        return reply(response, capacity, size, "no trigger");
    }
    const esp_err_t err = s.trigger();
    if (err == ESP_ERR_INVALID_STATE) {
        return reply(response, capacity, size, "busy");
    }
    if (err != ESP_OK) {
        return reply(response, capacity, size, "refused 0x%x", (int)err);
    }
    /* Returns as soon as the worker has the job. A query outlives any sane RPC
     * deadline, so the result is collected with method 4. */
    return reply(response, capacity, size, "queued");
}

static esp_err_t handle_stats(const esp_iris_rpc_request_t *request, uint8_t *response,
                              size_t capacity, size_t *size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    if (!s.lock) {
        return reply(response, capacity, size, "{}");
    }
    /* Formatted under the lock rather than copied to a local first: the
     * snapshot is over 200 bytes and this task cannot afford it. The critical
     * section is one vsnprintf and the writer is a single worker. */
    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err;
    if (!s.have_snapshot) {
        err = reply(response, capacity, size, "{\"seq\":0}");
    } else {
        /* JSON on purpose. These lines get appended straight into
         * measurements/raw and read back by a plotting script. The answer text
         * is last so a truncated reply still carries every number. */
        err = reply(response, capacity, size,
                    "{\"seq\":%u,\"err\":%d,\"http\":%d,\"jpeg_bytes\":%u,"
                    "\"sent_bytes\":%u,\"recv_bytes\":%u,\"encode_us\":%u,"
                    "\"connect_ms\":%u,\"request_ms\":%u,\"response_ms\":%u,"
                    "\"total_ms\":%u,\"rssi\":%d,\"prompt_tokens\":%d,"
                    "\"output_tokens\":%d,\"text\":\"%s\"}",
                    (unsigned)s.seq, (int)s.status, s.result.http_status,
                    (unsigned)s.jpeg_bytes, (unsigned)s.result.sent_bytes,
                    (unsigned)s.result.recv_bytes, (unsigned)s.encode_us,
                    (unsigned)s.connect_ms, (unsigned)s.result.request_ms,
                    (unsigned)s.result.response_ms, (unsigned)s.result.total_ms,
                    (int)s.rssi, s.result.prompt_tokens, s.result.output_tokens,
                    s.result.text);
    }
    xSemaphoreGive(s.lock);
    return err;
}

static esp_err_t handle_page(const esp_iris_rpc_request_t *request, uint8_t *response,
                             size_t capacity, size_t *size, void *user_ctx)
{
    (void)user_ctx;

    static const struct {
        const char *name;
        partid_page_t page;
    } PAGES[] = {
        { "scan",     PARTID_PAGE_SCAN },
        { "settings", PARTID_PAGE_SETTINGS },
        { "wifi",     PARTID_PAGE_WIFI },
        { "text",     PARTID_PAGE_TEXT },
    };

    if (!request->payload || request->payload_size == 0 ||
        request->payload_size >= sizeof(s.line)) {
        return reply(response, capacity, size, "expected one of scan settings wifi text");
    }
    memcpy(s.line, request->payload, request->payload_size);
    s.line[request->payload_size] = '\0';

    for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); ++i) {
        if (strcmp(s.line, PAGES[i].name) == 0) {
            partid_ui_show(PAGES[i].page);
            return reply(response, capacity, size, "ok %s", PAGES[i].name);
        }
    }
    return reply(response, capacity, size, "unknown page %s", s.line);
}

static esp_err_t handle_portal(const esp_iris_rpc_request_t *request, uint8_t *response,
                               size_t capacity, size_t *size, void *user_ctx)
{
    (void)user_ctx;

    bool want = !partid_portal_is_running();
    if (request->payload && request->payload_size) {
        const char c = (char)request->payload[0];
        want = (c == '1' || c == 'o' || c == 'y');
    }

    const esp_err_t err = want ? partid_portal_start() : partid_portal_stop();
    if (err != ESP_OK) {
        return reply(response, capacity, size, "failed 0x%x", (int)err);
    }
    if (!want) {
        return reply(response, capacity, size, "portal off");
    }

    char ssid[33];
    char pass[33];
    (void)partid_portal_ssid(ssid, sizeof(ssid));
    (void)partid_portal_password(pass, sizeof(pass));
    return reply(response, capacity, size, "portal on ssid=%s pass=%s url=http://192.168.4.1/",
                 ssid, pass);
}

esp_err_t partid_rpc_register(partid_rpc_trigger_fn trigger)
{
    s.trigger = trigger;
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    const struct {
        uint16_t method;
        esp_iris_rpc_handler_t handler;
    } methods[] = {
        { PARTID_RPC_METHOD_SET,    handle_set },
        { PARTID_RPC_METHOD_STATUS, handle_status },
        { PARTID_RPC_METHOD_QUERY,  handle_query },
        { PARTID_RPC_METHOD_STATS,  handle_stats },
        { PARTID_RPC_METHOD_PAGE,   handle_page },
        { PARTID_RPC_METHOD_PORTAL, handle_portal },
    };

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const esp_err_t err = esp_iris_rpc_register(PARTID_RPC_SERVICE_ID,
                                                    methods[i].method,
                                                    methods[i].handler, NULL);
        if (err != ESP_OK) {
            /*
             * Not fatal, deliberately. This used to return the error into an
             * ESP_ERROR_CHECK, so one method too many aborted the whole
             * application and the board fell back to Vibe Mode on the next
             * boot. Losing one RPC method is worth a log line; losing the
             * application is not.
             */
            ESP_LOGE(TAG, "method %u did not register, 0x%x", (unsigned)methods[i].method,
                     (int)err);
        }
    }
    ESP_LOGI(TAG, "RPC service %u ready, methods 1 set, 2 status, 3 query, 4 stats, 5 page, 6 portal",
             (unsigned)PARTID_RPC_SERVICE_ID);
    return ESP_OK;
}

void partid_rpc_publish(uint32_t seq, esp_err_t status,
                        const partid_vision_result_t *result,
                        uint32_t connect_ms, uint32_t encode_us,
                        size_t jpeg_bytes, int8_t rssi)
{
    if (!s.lock || !result) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.have_snapshot = true;
    s.seq = seq;
    s.status = status;
    s.result = *result;
    s.connect_ms = connect_ms;
    s.encode_us = encode_us;
    s.jpeg_bytes = jpeg_bytes;
    s.rssi = rssi;
    xSemaphoreGive(s.lock);
}
