// SPDX-License-Identifier: Apache-2.0

#include "partid_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define NET_UP_BIT   BIT0
#define NET_FAIL_BIT BIT1
#define NET_ATTEMPTS 5

static const char *TAG = "partid_net";

static struct {
    EventGroupHandle_t events;
    esp_netif_t *netif;
    bool inited;
    bool started;
    bool want_up;
    volatile bool up;
    int attempts;
    int64_t t0;
} s;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /*
         * Only when a connection was actually asked for. Associating from
         * here unconditionally is the usual example code and it quietly breaks
         * scanning: starting the driver in order to scan immediately begins a
         * connection instead, and the driver then refuses the scan with
         * "STA is connecting, scan are not allowed".
         */
        if (s.want_up) {
            (void)esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.up = false;
        if (!s.want_up) {
            return; /* our own disconnect, not a failure */
        }
        const wifi_event_sta_disconnected_t *event = data;
        const int reason = event ? (int)event->reason : 0;
        if (++s.attempts <= NET_ATTEMPTS) {
            ESP_LOGW(TAG, "disconnected, reason %d, attempt %d", reason, s.attempts);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "gave up after %d attempts, reason %d", s.attempts, reason);
            xEventGroupSetBits(s.events, NET_FAIL_BIT);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        s.up = true;
        s.attempts = 0;
        ESP_LOGI(TAG, "up at " IPSTR " in %u ms", IP2STR(&event->ip_info.ip),
                 (unsigned)((esp_timer_get_time() - s.t0) / 1000));
        xEventGroupSetBits(s.events, NET_UP_BIT);
    }
}

esp_err_t partid_net_init(void)
{
    if (s.inited) {
        return ESP_OK;
    }

    /* ESP-Iris brings up its own TCP transport once a netif exists, so both of
     * these may already be done. Both report INVALID_STATE rather than
     * failing, which is the only safe way to share them. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s.events = xEventGroupCreate();
    if (!s.events) {
        return ESP_ERR_NO_MEM;
    }
    s.netif = esp_netif_create_default_wifi_sta();
    if (!s.netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL),
                        TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    /* Credentials live in our own NVS namespace. Letting the driver cache a
     * second copy would mean two places to clear. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");

    s.inited = true;
    return ESP_OK;
}

esp_err_t partid_net_connect(const char *ssid, const char *pass,
                             uint32_t timeout_ms, uint32_t *out_ms)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "init");

    if (out_ms) {
        *out_ms = 0;
    }
    if (s.up) {
        return ESP_OK;
    }

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    if (pass) {
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    }
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "sta config");

    xEventGroupClearBits(s.events, NET_UP_BIT | NET_FAIL_BIT);
    s.attempts = 0;
    s.want_up = true;
    s.t0 = esp_timer_get_time();

    /* esp_wifi_start() raises STA_START, which associates. After a disconnect
     * the driver is still started, so associate directly instead. */
    esp_err_t err;
    if (!s.started) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s.started = true;
        }
    } else {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        s.want_up = false;
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s.events, NET_UP_BIT | NET_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & NET_UP_BIT)) {
        s.want_up = false;
        (void)esp_wifi_disconnect();
        return (bits & NET_FAIL_BIT) ? ESP_ERR_WIFI_NOT_CONNECT : ESP_ERR_TIMEOUT;
    }

    if (out_ms) {
        *out_ms = (uint32_t)((esp_timer_get_time() - s.t0) / 1000);
    }
    return ESP_OK;
}

esp_err_t partid_net_ensure_started(void)
{
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "init");
    if (s.started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s.started = true;
    return ESP_OK;
}

esp_err_t partid_net_scan(partid_net_ap_t *out, size_t max, size_t *out_count)
{
    if (!out || !max || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "init");

    /* Starting without want_up set means the driver comes up idle rather than
     * associating, which is what lets the user browse networks first. */
    ESP_RETURN_ON_ERROR(partid_net_ensure_started(), TAG, "start");

    /* An association attempt already in flight would refuse the scan, so stop
     * it. A completed association is fine and is left alone. */
    if (!s.up) {
        s.want_up = false;
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    const esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed 0x%x", (int)err);
        return err;
    }

    uint16_t found = 0;
    (void)esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }

    /* The driver's record is large, so the raw list is read in one allocation
     * from PSRAM and reduced immediately. */
    const uint16_t want = found > 32 ? 32 : found;
    wifi_ap_record_t *records =
        heap_caps_calloc(want, sizeof(*records), MALLOC_CAP_SPIRAM);
    if (!records) {
        (void)esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = want;
    esp_err_t read_err = esp_wifi_scan_get_ap_records(&got, records);
    if (read_err == ESP_OK) {
        for (uint16_t i = 0; i < got && *out_count < max; ++i) {
            const char *name = (const char *)records[i].ssid;
            if (!name[0]) {
                continue; /* hidden */
            }
            bool seen = false;
            for (size_t j = 0; j < *out_count; ++j) {
                if (strcmp(out[j].ssid, name) == 0) {
                    seen = true; /* same network on another band or radio */
                    break;
                }
            }
            if (seen) {
                continue;
            }
            snprintf(out[*out_count].ssid, sizeof(out[*out_count].ssid), "%s", name);
            out[*out_count].rssi = records[i].rssi;
            out[*out_count].secure = records[i].authmode != WIFI_AUTH_OPEN;
            ++(*out_count);
        }
    }

    free(records);
    ESP_LOGI(TAG, "scan found %u, kept %u", (unsigned)found, (unsigned)*out_count);
    return read_err;
}

esp_err_t partid_net_disconnect(void)
{
    if (!s.inited || !s.started) {
        return ESP_OK;
    }
    s.want_up = false;
    s.up = false;
    xEventGroupClearBits(s.events, NET_UP_BIT | NET_FAIL_BIT);
    return esp_wifi_disconnect();
}

bool partid_net_is_up(void) { return s.up; }

int8_t partid_net_rssi(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
