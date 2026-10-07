// SPDX-License-Identifier: Apache-2.0

#include "partid_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "partid_camera.h"
#include "partid_config.h"
#include "partid_net.h"
#include "partid_portal.h"

#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"

static const char *TAG = "partid_ui";

#define WIFI_SLOTS 8
#define UI_TASK_STACK 4096

/* Cycled by tapping the Model row. Short list on purpose: these are the ones
 * that answer, and gemini-2.5-flash is not among them because a new API key
 * gets 404 from it. */
static const char *const MODELS[] = {
    "gemini-3.1-flash-lite",
    "gemini-3.8-flash",
    "gemini-3.5-flash",
    "gemini-flash-latest",
};
#define MODEL_COUNT (sizeof(MODELS) / sizeof(MODELS[0]))

/* What the keyboard is currently editing. */
typedef enum {
    EDIT_NONE = 0,
    EDIT_PASSWORD,
    EDIT_KEY,
} edit_target_t;

/*
 * Single owner per field.
 *
 *   page, aps, ap_count  written by the UI worker and the GSP event task,
 *                        read by the row binder on the render task. The rows
 *                        are only rebound after a scan completes, and a stale
 *                        read there costs a wrong label for one frame, so
 *                        these are plain rather than locked.
 */
static struct {
    esp_gsp_handle_t ui;
    partid_page_t page;
    SemaphoreHandle_t scan_job;
    esp_gsp_list_t list;
    bool list_bound;

    partid_net_ap_t aps[WIFI_SLOTS];
    size_t ap_count;
    char pending_ssid[33];

    edit_target_t editing;
} s;

/* Defined with the wifi page below; used by the page switch above it. */
static void start_scan(void);

/* ------------------------------------------------------------------ values */

/*
 * The join details belong on the screen, because there is nowhere else a user
 * could read them: the password is derived per device so it is not printable
 * on a sticker, and the board is the only thing in the room that knows it.
 */
static void refresh_portal_info(void)
{
    if (!s.ui) {
        return;
    }
    if (!partid_portal_is_running()) {
        (void)gsp_partid_portal_info_set_text(s.ui, "");
        return;
    }
    char ssid[33];
    char pass[33];
    char line[160]; /* room for a 32 char name and a 32 char key */
    if (partid_portal_ssid(ssid, sizeof(ssid)) != ESP_OK ||
        partid_portal_password(pass, sizeof(pass)) != ESP_OK) {
        return;
    }
    /* Two lines. The label wraps on the newline, and 10 is written as a
     * number because the join details are the only place this file needs
     * one. */
    snprintf(line, sizeof(line),
             "Join %s%cpassword %s, then http://192.168.4.1",
             ssid, 10, pass);
    (void)gsp_partid_portal_info_set_text(s.ui, line);
}

static void refresh_settings(void)
{
    partid_config_t cfg;
    if (!s.ui || partid_config_load(&cfg) != ESP_OK) {
        return;
    }

    char buf[48];
    (void)gsp_partid_val_wifi_set_text(s.ui, cfg.ssid[0] ? cfg.ssid : "not set");
    (void)gsp_partid_val_key_set_text(s.ui, cfg.api_key[0] ? "set" : "not set");

    /* The names share a long prefix, so the prefix is dropped. */
    const char *model = cfg.model;
    if (strncmp(model, "gemini-", 7) == 0) {
        model += 7;
    }
    (void)gsp_partid_val_model_set_text(s.ui, model);

    snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.rotation);
    (void)gsp_partid_val_rot_set_text(s.ui, buf);

    (void)gsp_partid_val_mirror_set_text(s.ui, cfg.mirror ? "on" : "off");
    (void)gsp_partid_val_portal_set_text(s.ui, partid_portal_is_running() ? "on" : "off");
    refresh_portal_info();
}

/* ------------------------------------------------------------- page switch */

void partid_ui_show(partid_page_t page)
{
    if (!s.ui) {
        return;
    }
    s.page = page;

    (void)gsp_partid_page_scan_set_visible(s.ui, page == PARTID_PAGE_SCAN);
    (void)gsp_partid_page_settings_set_visible(s.ui, page == PARTID_PAGE_SETTINGS);
    (void)gsp_partid_page_wifi_set_visible(s.ui, page == PARTID_PAGE_WIFI);
    (void)gsp_partid_page_text_set_visible(s.ui, page == PARTID_PAGE_TEXT);

    if (page == PARTID_PAGE_SETTINGS) {
        refresh_settings();
    } else if (page == PARTID_PAGE_WIFI) {
        /* Here rather than in the tap handler, so arriving by RPC scans too.
         * The job is a binary semaphore, so asking twice costs nothing. */
        start_scan();
    }
}

partid_page_t partid_ui_page(void) { return s.page; }

/* ------------------------------------------------------------- wifi list */

/* Render-task context. Keep it to formatting. */
static gsp_err_t bind_row(esp_gsp_handle_t gsp, esp_gsp_row_t row,
                          uint32_t index, void *ctx)
{
    (void)ctx;
    char line[64];
    if (index < s.ap_count) {
        snprintf(line, sizeof(line), "%s   %d dBm%s", s.aps[index].ssid,
                 (int)s.aps[index].rssi, s.aps[index].secure ? "" : "  open");
    } else {
        line[0] = '\0';
    }
    return esp_gsp_row_text(gsp, row, line);
}

static void publish_scan(void)
{
    if (!s.ui) {
        return;
    }
    if (!s.list_bound) {
        s.list = gsp_partid_wifi_list_bind(s.ui, bind_row, NULL);
        s.list_bound = true;
    }
    (void)gsp_partid_wifi_list_set_total(s.ui, s.list, (uint32_t)s.ap_count);
    (void)gsp_partid_wifi_list_refresh(s.ui, s.list);

    char hint[48];
    if (s.ap_count == 0) {
        snprintf(hint, sizeof(hint), "No networks found");
    } else {
        snprintf(hint, sizeof(hint), "%u networks, tap one",
                 (unsigned)s.ap_count);
    }
    (void)gsp_partid_wifi_hint_set_text(s.ui, hint);
}

/*
 * A scan blocks for seconds, so it cannot run on the event task that delivered
 * the tap. The worker exists for that one reason.
 */
static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s.scan_job, portMAX_DELAY);

        (void)gsp_partid_wifi_hint_set_text(s.ui, "Scanning...");
        size_t count = 0;
        const esp_err_t err = partid_net_scan(s.aps, WIFI_SLOTS, &count);
        s.ap_count = (err == ESP_OK) ? count : 0;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan failed 0x%x", (int)err);
        }
        publish_scan();
    }
}

static void start_scan(void)
{
    if (s.scan_job) {
        xSemaphoreGive(s.scan_job);
    }
}

/* ------------------------------------------------------------ text entry */

static void open_editor(edit_target_t target, const char *title)
{
    s.editing = target;
    (void)gsp_partid_text_title_set_text(s.ui, title);
    (void)gsp_partid_text_value_set_text(s.ui, "");

    /* Re-attaching resets the edit buffer, which is what makes the field start
     * empty rather than holding the previous secret. */
    const esp_gsp_err_t err = esp_gsp_keyboard_attach(
        s.ui, GSP_PARTID_ACT_ID_KB_KEY, GSP_PARTID_BIND_TEXT_VALUE);
    if (err != ESP_GSP_OK) {
        ESP_LOGE(TAG, "keyboard attach failed %d", (int)err);
    }
    partid_ui_show(PARTID_PAGE_TEXT);
}

/* OK was pressed: the field already holds the final text. */
static void commit_editor(void)
{
    char text[PARTID_KEY_CAP];
    if (esp_gsp_keyboard_text(s.ui, text, sizeof(text)) != ESP_GSP_OK) {
        ESP_LOGW(TAG, "could not read the edited text");
        return;
    }

    switch (s.editing) {
    case EDIT_PASSWORD:
        if (s.pending_ssid[0]) {
            (void)partid_config_set("ssid", s.pending_ssid);
            (void)partid_config_set("pass", text);
        }
        break;
    case EDIT_KEY:
        (void)partid_config_set("gkey", text);
        break;
    default:
        break;
    }

    s.editing = EDIT_NONE;
    /* Detach so stray keys cannot keep editing a field nobody is looking at. */
    (void)esp_gsp_keyboard_attach(s.ui, ESP_GSP_KEYBOARD_NONE,
                                  GSP_PARTID_BIND_TEXT_VALUE);
    partid_ui_show(PARTID_PAGE_SETTINGS);
}

/* --------------------------------------------------------------- events */

static void cycle_model(void)
{
    partid_config_t cfg;
    if (partid_config_load(&cfg) != ESP_OK) {
        return;
    }
    size_t next = 0;
    for (size_t i = 0; i < MODEL_COUNT; ++i) {
        if (strcmp(cfg.model, MODELS[i]) == 0) {
            next = (i + 1) % MODEL_COUNT;
            break;
        }
    }
    (void)partid_config_set("model", MODELS[next]);
    refresh_settings();
}

/*
 * Rotation is deliberately not cycled from here.
 *
 * It was, and the result was the worst kind of bug: three taps walked the
 * value from 90 back round to 270, the preview looked plausible at a glance,
 * and every identification came back as garbled characters. That reads as a
 * failing model or a failing camera, and the actual cause is a setting nobody
 * remembers touching. The sensor's mounting does not change, so this is not a
 * value that belongs behind a tap on the main device UI.
 */
static void explain_rotation(void)
{
    if (!s.ui || partid_portal_is_running()) {
        return; /* the portal's own join details own that line */
    }
    (void)gsp_partid_portal_info_set_text(
        s.ui, "Rotation is fixed by how the sensor is mounted."
              " Change it from the setup portal if you must.");
}

static void toggle_mirror(void)
{
    const bool on = !partid_camera_mirror_is_on();
    partid_camera_set_mirror(on);
    (void)partid_config_set("flip", on ? "1" : "0");
    refresh_settings();
}

bool partid_ui_handle_event(const esp_gsp_event_t *event)
{
    if (!s.ui || !event) {
        return false;
    }

    if (gsp_partid_event_is_settings(event)) {
        partid_ui_show(PARTID_PAGE_SETTINGS);
        return true;
    }
    if (gsp_partid_event_is_close_settings(event)) {
        partid_ui_show(PARTID_PAGE_SCAN);
        return true;
    }
    if (gsp_partid_event_is_open_wifi(event)) {
        partid_ui_show(PARTID_PAGE_WIFI);
        return true;
    }
    if (gsp_partid_event_is_close_wifi(event)) {
        partid_ui_show(PARTID_PAGE_SETTINGS);
        return true;
    }
    if (gsp_partid_event_is_rescan(event)) {
        start_scan();
        return true;
    }
    if (gsp_partid_event_is_pick_wifi(event)) {
        uint32_t index = 0;
        if (gsp_partid_wifi_list_get_selected(s.ui, &index) == ESP_OK &&
            index < s.ap_count) {
            snprintf(s.pending_ssid, sizeof(s.pending_ssid), "%s",
                     s.aps[index].ssid);
            char title[64];
            snprintf(title, sizeof(title), "Password for %s", s.pending_ssid);
            open_editor(EDIT_PASSWORD, title);
        }
        return true;
    }
    if (gsp_partid_event_is_open_key(event)) {
        open_editor(EDIT_KEY, "Paste or type the API key");
        return true;
    }
    if (gsp_partid_event_is_close_text(event)) {
        s.editing = EDIT_NONE;
        (void)esp_gsp_keyboard_attach(s.ui, ESP_GSP_KEYBOARD_NONE,
                                      GSP_PARTID_BIND_TEXT_VALUE);
        partid_ui_show(PARTID_PAGE_SETTINGS);
        return true;
    }
    if (gsp_partid_event_is_kb_key(event)) {
        /* Only OK reaches here; character keys are handled inside GSP. */
        commit_editor();
        return true;
    }
    if (gsp_partid_event_is_next_model(event)) {
        cycle_model();
        return true;
    }
    if (gsp_partid_event_is_next_rot(event)) {
        explain_rotation();
        return true;
    }
    if (gsp_partid_event_is_portal(event)) {
        /* Starting the access point and the server takes a moment and runs
         * here on the event task, which is acceptable because nothing else is
         * happening while a user is reading a settings screen. */
        const esp_err_t err = partid_portal_is_running() ? partid_portal_stop()
                                                         : partid_portal_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "portal toggle failed 0x%x", (int)err);
            (void)gsp_partid_val_portal_set_text(s.ui, "failed");
        } else {
            refresh_settings();
        }
        return true;
    }

    /* Mirror is reachable from the scan page and the settings row, and both
     * send the same callback, so it is handled here for both. */
    if (gsp_partid_event_is_flip(event)) {
        toggle_mirror();
        return s.page != PARTID_PAGE_SCAN;
    }

    return false;
}

esp_err_t partid_ui_init(esp_gsp_handle_t ui)
{
    if (!ui) {
        return ESP_ERR_INVALID_ARG;
    }
    s.ui = ui;
    s.page = PARTID_PAGE_SCAN;

    s.scan_job = xSemaphoreCreateBinary();
    if (!s.scan_job) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ui_task, "partid_ui", UI_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    partid_ui_show(PARTID_PAGE_SCAN);
    return ESP_OK;
}
