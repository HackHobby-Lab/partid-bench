// SPDX-License-Identifier: Apache-2.0

#include "partid_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "partid_config.h"
#include "partid_net.h"

static const char *TAG = "partid_portal";

#define PORTAL_CHANNEL     6
#define PORTAL_MAX_CONN    2
#define PORTAL_BODY_MAX    2048
#define PORTAL_PAGE_MAX    8192

static struct {
    httpd_handle_t server;
    esp_netif_t *ap_netif;
    bool running;
} s;

/* ------------------------------------------------------------- identity */

/*
 * Named and keyed from the MAC so two boards on a bench do not collide and so
 * no secret is baked into a publishable image. The MAC is broadcast anyway, so
 * this is not a strong password; it keeps a neighbour off the form, and the
 * portal only exists while someone is standing at the board.
 */
static esp_err_t device_suffix(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "mac");
    snprintf(out, cap, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t partid_portal_ssid(char *out, size_t cap)
{
    char suffix[8];
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "net init");
    ESP_RETURN_ON_ERROR(device_suffix(suffix, sizeof(suffix)), TAG, "suffix");
    snprintf(out, cap, "partid-%s", suffix);
    return ESP_OK;
}

esp_err_t partid_portal_password(char *out, size_t cap)
{
    char suffix[8];
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "net init");
    ESP_RETURN_ON_ERROR(device_suffix(suffix, sizeof(suffix)), TAG, "suffix");
    snprintf(out, cap, "mosaico%s", suffix);
    return ESP_OK;
}

/* ------------------------------------------------------------ form input */

/* application/x-www-form-urlencoded, decoded in place. */
static void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/*
 * Pull one field out of a form body. Returns false when the field is absent,
 * which is different from present and empty: absent leaves the stored value
 * alone, so a blank password box does not wipe a working one.
 */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", key);
    const size_t len = strlen(needle);

    for (const char *p = body; p && *p;) {
        if (strncmp(p, needle, len) == 0) {
            const char *end = strchr(p, '&');
            size_t n = end ? (size_t)(end - p - len) : strlen(p + len);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, p + len, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

/* --------------------------------------------------------------- the page */

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Part ID setup</title><style>"
    "body{background:#07090C;color:#E8EAED;font:16px system-ui,sans-serif;margin:0;padding:24px}"
    "h1{font-size:20px;margin:0 0 4px}p.sub{color:#8B939E;font-size:13px;margin:0 0 20px}"
    "label{display:block;margin:16px 0 6px;font-size:13px;color:#8B939E}"
    "input,select{width:100%;box-sizing:border-box;background:#1E222A;border:1px solid #2A313B;"
    "color:#E8EAED;border-radius:10px;padding:12px;font-size:16px}"
    "button{width:100%;margin-top:24px;background:#FF4C01;color:#fff;border:0;"
    "border-radius:10px;padding:14px;font-size:16px;font-weight:600}"
    "small{color:#8B939E;display:block;margin-top:6px;font-size:12px}"
    ".ok{background:#132; border:1px solid #27C498;color:#27C498;padding:12px;"
    "border-radius:10px;margin-bottom:16px}"
    "</style></head><body><h1>Part ID setup</h1>"
    "<p class=sub>Settings are stored on the board and survive an update.</p>";

static const char PAGE_TAIL[] =
    "<button type=submit>Save</button></form></body></html>";

/* ------------------------------------------------------------- handlers */

static esp_err_t get_root(httpd_req_t *req)
{
    partid_config_t cfg;
    if (partid_config_load(&cfg) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    char *page = heap_caps_malloc(PORTAL_PAGE_MAX, MALLOC_CAP_SPIRAM);
    if (!page) {
        return httpd_resp_send_500(req);
    }

    const bool saved = strstr(req->uri, "saved") != NULL;
    int n = snprintf(page, PORTAL_PAGE_MAX,
        "%s%s<form method=post action=/save>"
        "<label>Wi-Fi network</label>"
        "<input name=ssid value=\"%s\" placeholder='network name'>"
        "<label>Wi-Fi password</label>"
        "<input name=pass type=password placeholder='%s'>"
        "<small>Leave blank to keep the stored password.</small>"
        "<label>Gemini API key</label>"
        "<input name=gkey type=password placeholder='%s'>"
        "<small>Leave blank to keep the stored key.</small>"
        "<label>Model</label>"
        "<select name=model>"
        "<option%s>gemini-3.1-flash-lite</option>"
        "<option%s>gemini-3.8-flash</option>"
        "<option%s>gemini-3.5-flash</option>"
        "<option%s>gemini-flash-latest</option>"
        "</select>"
        "<small>The free tier allows 20 requests a day per model.</small>"
        "<label>Rotation</label>"
        "<select name=rot>"
        "<option%s>0</option><option%s>90</option>"
        "<option%s>180</option><option%s>270</option></select>"
        "<label>Mirror</label>"
        "<select name=flip><option value=0%s>off</option>"
        "<option value=1%s>on</option></select>"
        "<label>Drop wifi after each query</label>"
        "<select name=cycle><option value=0%s>no</option>"
        "<option value=1%s>yes</option></select>"
        "<small>Changes how much charge one query costs.</small>"
        "%s",
        PAGE_HEAD,
        saved ? "<div class=ok>Saved.</div>" : "",
        cfg.ssid,
        cfg.pass[0] ? "stored, leave blank to keep" : "not set",
        cfg.api_key[0] ? "stored, leave blank to keep" : "not set",
        strcmp(cfg.model, "gemini-3.1-flash-lite") == 0 ? " selected" : "",
        strcmp(cfg.model, "gemini-3.8-flash") == 0 ? " selected" : "",
        strcmp(cfg.model, "gemini-3.5-flash") == 0 ? " selected" : "",
        strcmp(cfg.model, "gemini-flash-latest") == 0 ? " selected" : "",
        cfg.rotation == 0 ? " selected" : "",
        cfg.rotation == 90 ? " selected" : "",
        cfg.rotation == 180 ? " selected" : "",
        cfg.rotation == 270 ? " selected" : "",
        cfg.mirror ? "" : " selected",
        cfg.mirror ? " selected" : "",
        cfg.cycle_wifi ? "" : " selected",
        cfg.cycle_wifi ? " selected" : "",
        PAGE_TAIL);

    esp_err_t err;
    if (n < 0 || n >= PORTAL_PAGE_MAX) {
        ESP_LOGE(TAG, "page did not fit");
        err = httpd_resp_send_500(req);
    } else {
        httpd_resp_set_type(req, "text/html");
        err = httpd_resp_send(req, page, n);
    }
    free(page);
    return err;
}

static esp_err_t post_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= PORTAL_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    char *body = heap_caps_calloc(1, PORTAL_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        return httpd_resp_send_500(req);
    }

    int got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        got += n;
    }
    body[got] = '\0';

    /* One buffer, reused per field, sized for the longest of them. */
    char value[PARTID_KEY_CAP];
    static const char *const KEYS[] = { "ssid", "model", "rot", "flip", "cycle" };

    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); ++i) {
        if (form_field(body, KEYS[i], value, sizeof(value))) {
            (void)partid_config_set(KEYS[i], value);
        }
    }
    /* Secrets only when actually typed, so a blank box keeps what is stored. */
    if (form_field(body, "pass", value, sizeof(value)) && value[0]) {
        (void)partid_config_set("pass", value);
    }
    if (form_field(body, "gkey", value, sizeof(value)) && value[0]) {
        (void)partid_config_set("gkey", value);
    }

    free(body);
    ESP_LOGI(TAG, "settings saved from the portal");

    /* Redirect rather than render, so a refresh does not resubmit. */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved");
    return httpd_resp_send(req, NULL, 0);
}

/* Anything else goes to the form, which is what a captive portal probe wants
 * and what a mistyped path should do anyway. */
static esp_err_t get_any(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------- lifecycle */

esp_err_t partid_portal_start(void)
{
    if (s.running) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(partid_net_init(), TAG, "net init");

    if (!s.ap_netif) {
        s.ap_netif = esp_netif_create_default_wifi_ap();
        if (!s.ap_netif) {
            return ESP_FAIL;
        }
    }

    char ssid[33];
    char pass[33];
    ESP_RETURN_ON_ERROR(partid_portal_ssid(ssid, sizeof(ssid)), TAG, "ssid");
    ESP_RETURN_ON_ERROR(partid_portal_password(pass, sizeof(pass)), TAG, "pass");

    wifi_config_t ap = {0};
    /* ap.ssid is 32 bytes and holds no terminator, so the length is explicit
     * and copied rather than printed. */
    const size_t ssid_len = strnlen(ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", pass);
    ap.ap.channel = PORTAL_CHANNEL;
    ap.ap.max_connection = PORTAL_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /* APSTA, not AP. Dropping the station to serve a form would disconnect a
     * board that is mid-measurement. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    ESP_RETURN_ON_ERROR(partid_net_ensure_started(), TAG, "wifi start");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 5120;
    ESP_RETURN_ON_ERROR(httpd_start(&s.server, &cfg), TAG, "httpd");

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    const httpd_uri_t any  = { .uri = "/*", .method = HTTP_GET, .handler = get_any };
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &root));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &save));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &any));

    s.running = true;
    ESP_LOGI(TAG, "portal up: SSID %s, password %s, http://192.168.4.1/", ssid, pass);
    return ESP_OK;
}

esp_err_t partid_portal_stop(void)
{
    if (!s.running) {
        return ESP_OK;
    }
    if (s.server) {
        (void)httpd_stop(s.server);
        s.server = NULL;
    }
    /* Back to station only, which also frees the AP's buffers. */
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    s.running = false;
    ESP_LOGI(TAG, "portal down");
    return ESP_OK;
}

bool partid_portal_is_running(void) { return s.running; }
