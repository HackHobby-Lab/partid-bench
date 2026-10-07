// SPDX-License-Identifier: Apache-2.0

#include "partid_vision.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char *TAG = "partid_vision";

/* A multiple of 3, so base64 padding appears exactly once, at the very end. */
#define B64_IN_CHUNK  3072U
#define B64_OUT_CHUNK ((((B64_IN_CHUNK + 2U) / 3U) * 4U) + 1U)
#define RESPONSE_CAP  4096U
/*
 * 20 s was not enough and the failure was invisible: the body went out fine,
 * then the socket sat silent for exactly 20003 ms and the query was recorded
 * as a transport error with no status code.
 *
 * The same image from a desktop on wired internet takes 17.5 s to come back,
 * so a vision response is simply slow, and the board spends 6 s uploading
 * before the clock even starts on the model. The timeout has to clear both.
 */
#define HTTP_TIMEOUT_MS 45000

/*
 * thinkingBudget 0 is asked for and is not granted.
 *
 * Measured against gemini-3.8-flash with this exact body: the response came
 * back with thoughtsTokenCount 73 despite a budget of zero, and those thinking
 * tokens are charged against maxOutputTokens. At 96 the thinking ate 73 of the
 * budget, the answer got 21, and finishReason was MAX_TOKENS, so the part
 * number arrived cut in half. The request looked successful and the answer was
 * quietly truncated, which is the worst shape a bug can have.
 *
 * So the budget stays at zero, because it costs nothing to ask and older
 * models honour it, and maxOutputTokens is raised far enough that unrequested
 * thinking cannot starve the answer.
 */
static const char BODY_TAIL[] =
    "\"}}]}],\"generationConfig\":{\"temperature\":0,\"maxOutputTokens\":512,"
    "\"thinkingConfig\":{\"thinkingBudget\":0}}}";

typedef struct {
    char url[192];
    char prompt[PARTID_PROMPT_CAP * 2];
    char head[PARTID_PROMPT_CAP * 2 + 160];
    char b64[B64_OUT_CHUNK];
    char response[RESPONSE_CAP];
} scratch_t;

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; ++in) {
        const unsigned char c = (unsigned char)*in;
        char unicode[7];
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(unicode, sizeof(unicode), "\\u%04x", c);
                esc = unicode;
            }
            break;
        }
        if (!esc) {
            out[n++] = (char)c;
            continue;
        }
        const size_t len = strlen(esc);
        if (n + len >= cap) {
            break;
        }
        memcpy(out + n, esc, len);
        n += len;
    }
    out[n] = '\0';
}

/*
 * The prompt forbids markdown and the model mostly obeys, but it still bolds a
 * part number now and then. The label has no markdown renderer, so a stray
 * asterisk would be drawn as an asterisk. 0x60 is the backtick.
 */
static void tidy(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ++r) {
        if (*r != 0x60 && *r != '*' && *r != '#') {
            *w++ = *r;
        }
    }
    *w = '\0';

    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\n' || s[lead] == '\r' || s[lead] == '\t') {
        ++lead;
    }
    if (lead) {
        memmove(s, s + lead, strlen(s + lead) + 1);
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static int json_int(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

esp_err_t partid_vision_identify(const partid_config_t *cfg,
                                 const uint8_t *jpeg, size_t jpeg_size,
                                 partid_vision_result_t *out)
{
    if (!cfg || !jpeg || !jpeg_size || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    scratch_t *sc = heap_caps_calloc(1, sizeof(scratch_t), MALLOC_CAP_SPIRAM);
    if (!sc) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_FAIL;
    cJSON *root = NULL;
    esp_http_client_handle_t client = NULL;
    int total = 0;

    snprintf(sc->url, sizeof(sc->url),
             "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent",
             cfg->model);

    json_escape(cfg->prompt, sc->prompt, sizeof(sc->prompt));
    const int head_len = snprintf(sc->head, sizeof(sc->head),
                                  "{\"contents\":[{\"parts\":[{\"text\":\"%s\"},"
                                  "{\"inline_data\":{\"mime_type\":\"image/jpeg\","
                                  "\"data\":\"",
                                  sc->prompt);
    if (head_len < 0 || (size_t)head_len >= sizeof(sc->head)) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    const size_t tail_len = strlen(BODY_TAIL);
    const size_t body_len = (size_t)head_len + 4U * ((jpeg_size + 2U) / 3U) + tail_len;

    const esp_http_client_config_t http = {
        .url = sc->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    client = esp_http_client_init(&http);
    if (!client) {
        err = ESP_FAIL;
        goto done;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    /* Header, not a query parameter. A key in the URL ends up in every log
     * line and every proxy access record that touches the request. */
    esp_http_client_set_header(client, "x-goog-api-key", cfg->api_key);

    const int64_t t_start = esp_timer_get_time();
    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed 0x%x", (int)err);
        goto done;
    }

    if (esp_http_client_write(client, sc->head, head_len) != head_len) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    out->sent_bytes = (size_t)head_len;

    for (size_t off = 0; off < jpeg_size; off += B64_IN_CHUNK) {
        const size_t remaining = jpeg_size - off;
        const size_t take = remaining < B64_IN_CHUNK ? remaining : B64_IN_CHUNK;
        size_t encoded = 0;
        if (mbedtls_base64_encode((unsigned char *)sc->b64, sizeof(sc->b64),
                                  &encoded, jpeg + off, take) != 0) {
            err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        if (esp_http_client_write(client, sc->b64, (int)encoded) != (int)encoded) {
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }
        out->sent_bytes += encoded;
    }

    if (esp_http_client_write(client, BODY_TAIL, (int)tail_len) != (int)tail_len) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    out->sent_bytes += tail_len;
    out->request_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);

    const int64_t t_response = esp_timer_get_time();
    (void)esp_http_client_fetch_headers(client);
    out->http_status = esp_http_client_get_status_code(client);

    while ((size_t)total < sizeof(sc->response) - 1 &&
           !esp_http_client_is_complete_data_received(client)) {
        const int n = esp_http_client_read(
            client, sc->response + total,
            (int)(sizeof(sc->response) - 1 - (size_t)total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    sc->response[total] = '\0';
    out->recv_bytes = (size_t)total;
    out->response_ms = (uint32_t)((esp_timer_get_time() - t_response) / 1000);
    out->total_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);

    if (out->http_status != 200) {
        /* The body carries the real reason, an expired key or a wrong model
         * name. Truncated because it is sometimes very long. */
        ESP_LOGE(TAG, "HTTP %d: %.*s", out->http_status, total > 240 ? 240 : total,
                 sc->response);
        /*
         * A bare status code on the screen tells the user nothing they can act
         * on, and the two that actually happen here have specific, fixable
         * causes: the free tier allows 20 requests per day per model, and the
         * model every tutorial names is refused to new keys.
         */
        const char *reason;
        switch (out->http_status) {
        case 429: reason = "Daily quota used up. Try another model"; break;
        case 404: reason = "Model unavailable. Change it in Settings"; break;
        case 503: reason = "Model busy. Try again"; break;
        case 400: reason = "Request rejected. Check the API key"; break;
        case 403: reason = "API key refused"; break;
        default:  reason = NULL; break;
        }
        if (reason) {
            snprintf(out->text, sizeof(out->text), "%s", reason);
        } else {
            snprintf(out->text, sizeof(out->text), "HTTP %d", out->http_status);
        }
        err = ESP_FAIL;
        goto done;
    }

    root = cJSON_Parse(sc->response);
    if (!root) {
        ESP_LOGE(TAG, "response was not JSON, %d bytes", total);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    {
        const cJSON *candidate =
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "candidates"), 0);
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(
                cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetObjectItemCaseSensitive(candidate, "content"), "parts"),
                0),
            "text");

        const cJSON *finish = cJSON_GetObjectItemCaseSensitive(candidate, "finishReason");
        const bool truncated = cJSON_IsString(finish) && finish->valuestring &&
                               strcmp(finish->valuestring, "MAX_TOKENS") == 0;

        if (cJSON_IsString(text) && text->valuestring) {
            snprintf(out->text, sizeof(out->text), "%s", text->valuestring);
            tidy(out->text);
            /* A cut answer still reads like an answer. On a tool whose whole
             * output is a part number, half a part number is worse than none,
             * so it has to be visible on the screen and not just in a log. */
            if (truncated) {
                ESP_LOGW(TAG, "answer truncated, thinking consumed the output budget");
                strlcat(out->text, " [cut]", sizeof(out->text));
            }
            err = ESP_OK;
        } else {
            /* A 200 with no text is a safety block or a token limit, and the
             * finish reason is the only way to tell which. */
            snprintf(out->text, sizeof(out->text), "No answer, %s",
                     cJSON_IsString(finish) && finish->valuestring ? finish->valuestring
                                                                  : "reason unknown");
            err = ESP_ERR_INVALID_RESPONSE;
        }

        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usageMetadata");
        out->prompt_tokens = json_int(usage, "promptTokenCount");
        out->output_tokens = json_int(usage, "candidatesTokenCount");
    }

done:
    cJSON_Delete(root);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(sc);
    return err;
}
