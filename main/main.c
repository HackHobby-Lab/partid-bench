// SPDX-License-Identifier: Apache-2.0
//
// Part ID. Point the camera at a component, tap, and it tells you what it is.
//
// Sensor to PPA to software JPEG to the screen, then the identical bytes to a
// cloud vision model over TLS, then the answer back on the same screen. The
// camera path is in partid_camera.c and the request is in partid_vision.c.
//
// Nothing secret is compiled in. The wifi password and the API key arrive over
// ESP-Iris RPC and live in NVS, so this image can be published as it is. See
// partid_rpc.h for the provisioning calls.
//
// The second half of this project is a measurement, not a demo, so the shape
// here is built around getting a clean one. Capture, wifi and request are
// timed separately. The camera and the encode loop are stopped before the
// radio starts. Every query emits one log line of nothing but numbers, and the
// same numbers come back over RPC as JSON, so a hundred queries can be run and
// collected without a hand on the board.

#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "partid_camera.h"
#include "partid_config.h"
#include "partid_net.h"
#include "partid_rpc.h"
#include "partid_ui.h"
#include "partid_vision.h"
#include "ui_bundle.h"

#include <stdio.h>
#include <string.h>

#include "bundle_gsp.h"

static const char *TAG = "partid";

/* Generous. A cold associate on a busy 2.4 GHz band is seconds, and calling
 * that a failure would throw away the measurement that explains why. */
#define WIFI_TIMEOUT_MS 15000

/* TLS with the certificate bundle is the deep part of this call chain. */
#define QUERY_TASK_STACK 10240

/* See the retry loop in app_main. */
#define CAMERA_START_ATTEMPTS 4
#define CAMERA_RETRY_DELAY_MS 1000

typedef enum {
    ST_LIVE,      /* preview running, waiting for a tap */
    ST_CAPTURING, /* a frame has been asked for */
    ST_QUERYING,  /* camera parked, radio working */
    ST_RESULT,    /* an answer or a failure is on screen */
} state_t;

/*
 * Module state, single owner per field.
 *
 *   s_state    written by the GSP event callback, the Iris service task and
 *              the query worker, read by all three. One word, and every
 *              transition is a move into a state only its own writer can
 *              leave, so it needs no lock.
 *   s_jpeg     written by the preview task before the semaphore is given and
 *              read by the worker after taking it. The semaphore is the
 *              handoff.
 */
static esp_gsp_handle_t s_ui;
static SemaphoreHandle_t s_job;
static uint8_t *s_jpeg;
static size_t s_jpeg_cap;
static volatile size_t s_jpeg_size;
static volatile state_t s_state = ST_LIVE;
static uint32_t s_seq;

/* Only the query worker touches these two, which is why they are this large
 * and still not on a stack. */
static partid_config_t s_cfg;
static partid_vision_result_t s_result;

static void set_status(const char *text)
{
    if (s_ui) {
        (void)gsp_partid_status_set_text(s_ui, text);
    }
}

/*
 * The answer panel is two rows, so something has to decide which row a string
 * belongs on.
 *
 * A newline settles it: the prompt asks for the part number on line 1 and what
 * the part is on line 2, so line 1 takes the big row whatever its length and
 * the rest takes the small one. Before this the whole answer went into one
 * label and the two ran together, which is the thing worth fixing: the part
 * number is what the user came for and it has to be findable without reading.
 *
 * Everything else that reaches here is a message rather than an answer, and
 * those are measured instead. A short one is a headline and belongs on the big
 * row. A long one is a sentence, would be cut by the ellipsis there, and goes
 * on the small row where it wraps.
 */
#define RESULT_HEADLINE_MAX 20

static void set_rows(const char *part, const char *desc)
{
    if (!s_ui) {
        return;
    }
    (void)gsp_partid_result_set_text(s_ui, part ? part : "");
    (void)gsp_partid_result_desc_set_text(s_ui, desc ? desc : "");
}

/* True for a leading word that reads like a part number, which is to say it
 * carries a digit. Used only to rescue an answer the model put on one line
 * when it was asked for two. */
static bool looks_like_part(const char *word, size_t len)
{
    if (len < 3) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (word[i] >= '0' && word[i] <= '9') {
            return true;
        }
    }
    return false;
}

static void set_result(const char *text)
{
    if (!s_ui) {
        return;
    }
    if (!text || !text[0]) {
        set_rows("", "");
        return;
    }

    /* Copied because the split needs a terminator in the middle of it and the
     * caller owns these bytes. Sized to the widest thing that reaches here. */
    char buf[sizeof(((partid_vision_result_t *)0)->text)];
    snprintf(buf, sizeof(buf), "%s", text);

    char *const nl = strchr(buf, '\n');
    if (nl) {
        /* A CRLF leaves the CR on the end of line 1, where the label draws it
         * as a box. */
        char *part_end = nl;
        while (part_end > buf && (part_end[-1] == '\r' || part_end[-1] == ' ')) {
            --part_end;
        }
        *part_end = '\0';

        char *desc = nl + 1;
        while (*desc == '\n' || *desc == '\r' || *desc == ' ') {
            ++desc;
        }
        /* Runtime bound text does not break on LF the way compiled text does,
         * which is what merged the two halves in the first place. The small
         * row wraps on its own, so any further line breaks become spaces and
         * fill the second line instead of being drawn or dropped. */
        for (char *c = desc; *c; ++c) {
            if (*c == '\n' || *c == '\r') {
                *c = ' ';
            }
        }
        set_rows(buf, desc);
        return;
    }

    if (strlen(buf) <= RESULT_HEADLINE_MAX) {
        set_rows(buf, "");
        return;
    }

    /* One line, too long for the headline row. If it opens with something
     * shaped like a part number the model simply used a space where it was
     * asked for a newline, so split there and the panel still reads right. */
    const char *const space = strchr(buf, ' ');
    if (space && looks_like_part(buf, (size_t)(space - buf))) {
        char *const cut = buf + (space - buf);
        *cut = '\0';
        set_rows(buf, cut + 1);
        return;
    }

    set_rows("", buf);
}

/*
 * The top bar answers "will this work if I press the button", which is the
 * only question a user has before pressing it. So it shows the network and
 * whether the key is set, not a generic title.
 */
static void refresh_topbar(void)
{
    if (!s_ui) {
        return;
    }
    partid_config_t cfg;
    if (partid_config_load(&cfg) != ESP_OK) {
        return;
    }

    (void)gsp_partid_net_name_set_text(s_ui, cfg.ssid[0] ? cfg.ssid : "No network");

    const char *state;
    if (!cfg.ssid[0]) {
        state = "tap the gear to set up";
    } else if (!cfg.api_key[0]) {
        state = "no API key";
    } else if (partid_net_is_up()) {
        state = "connected";
    } else {
        state = "ready";
    }
    (void)gsp_partid_net_state_set_text(s_ui, state);

    /* The model names are long and the bar is narrow, so show the part that
     * differs between them. */
    const char *tail = strrchr(cfg.model, '-');
    (void)gsp_partid_model_chip_set_text(s_ui, tail && tail[1] ? tail + 1 : cfg.model);
}

/*
 * The torch state is shown by filling the button rather than swapping the
 * glyph. set_image() wants raw encoded bytes, not a path, so swapping would
 * mean carrying both PNGs in the binary; and a filled button is what every
 * phone camera does anyway, so it needs no explaining.
 */
static void refresh_flash_button(void)
{
    if (s_ui) {
        (void)gsp_partid_btn_flash_set_color(
            s_ui, partid_camera_flash_is_on() ? 0xFF4C01 : 0x1E222A);
    }
}

/* Runs on the preview task, which has already returned its camera buffer. */
static void on_capture(const uint8_t *jpeg, size_t size, uint32_t width,
                       uint32_t height, void *ctx)
{
    (void)ctx;

    if (!s_jpeg || size == 0 || size > s_jpeg_cap) {
        ESP_LOGE(TAG, "capture of %u bytes does not fit %u", (unsigned)size,
                 (unsigned)s_jpeg_cap);
        set_status("Capture too large");
        s_state = ST_RESULT;
        return;
    }

    /* Copied because the callback owns these bytes only for this call, and the
     * query that follows takes seconds. */
    memcpy(s_jpeg, jpeg, size);
    s_jpeg_size = size;

    ESP_LOGI(TAG, "captured %ux%u, %u bytes", (unsigned)width, (unsigned)height,
             (unsigned)size);

    s_state = ST_QUERYING;
    set_status("Identifying");
    xSemaphoreGive(s_job);
}

/* Leaves an outcome on screen that a tap can clear. */
static void finish(const char *status, const char *result)
{
    set_status(status);
    if (result) {
        set_result(result);
    }
    s_state = ST_RESULT;
}

/* Shared by the touch handler and the RPC trigger. */
static esp_err_t begin_capture(void)
{
    /*
     * The resume result is checked rather than ignored, and this is why.
     * Waking the sensor is the one step between a tap and a capture that can
     * fail, and when it did, the state stayed at ST_CAPTURING with nothing
     * able to leave it. Every later trigger answered "busy" and the board
     * looked hung while it was in fact idle and healthy. A failure has to land
     * somewhere a tap can clear.
     */
    if (!s_jpeg) {
        /* One more go, now that the bus has had time to settle. Pressing the
         * shutter is what a user does anyway when nothing happens. */
        ESP_LOGI(TAG, "retrying the camera");
        set_status("Retrying the camera");
        if (partid_camera_start(s_ui, on_capture, NULL) != ESP_OK) {
            finish("Still no camera", "Check the LEFT module");
            return ESP_ERR_INVALID_STATE;
        }
        s_jpeg_cap = partid_camera_frame_capacity();
        s_jpeg = heap_caps_malloc(s_jpeg_cap, MALLOC_CAP_SPIRAM);
        if (!s_jpeg) {
            finish("Out of memory", "Reboot the board");
            return ESP_ERR_NO_MEM;
        }
        if (partid_config_load(&s_cfg) == ESP_OK) {
            partid_camera_set_rotation(s_cfg.rotation);
            partid_camera_set_mirror(s_cfg.mirror);
        }
    }

    s_state = ST_CAPTURING;
    set_status("Capturing");

    const esp_err_t err = partid_camera_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera resume failed 0x%x", (int)err);
        finish("Camera stalled", "Tap to retry");
        return err;
    }
    partid_camera_request_capture();
    return ESP_OK;
}

static void show_live(void)
{
    s_state = ST_LIVE;
    set_status("Point at a component");
    set_result("Tap to identify");
    (void)partid_camera_resume();
}

static void on_touch(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    (void)ctx;

    /* The setup pages take what belongs to them first. */
    if (partid_ui_handle_event(event)) {
        return;
    }

    if (gsp_partid_event_is_flash(event)) {
        (void)partid_camera_set_flash(!partid_camera_flash_is_on());
        refresh_flash_button();
        return;
    }

    if (!gsp_partid_event_is_shoot(event)) {
        return;
    }

    /* A tap that lands while a setup page is open is not a shutter press. */
    if (partid_ui_page() != PARTID_PAGE_SCAN) {
        return;
    }

    switch (s_state) {
    case ST_LIVE:
        (void)begin_capture();
        break;
    case ST_RESULT:
        show_live();
        break;
    default:
        break; /* a capture or a query is already in flight */
    }
}

/* Called on the ESP-Iris service task, so it only ever hands over a job. */
static esp_err_t on_rpc_query(void)
{
    if (s_state == ST_CAPTURING || s_state == ST_QUERYING) {
        return ESP_ERR_INVALID_STATE;
    }
    return begin_capture();
}

static void query_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_job, portMAX_DELAY);

        const size_t size = s_jpeg_size;
        const uint32_t seq = ++s_seq;
        const uint32_t encode_us = partid_camera_last_encode_us();

        /*
         * Quiet the board before the radio starts. The preview loop has
         * already parked itself; this stops the stream and sleeps the sensor.
         * Everything the trace shows after this line is the query.
         */
        (void)partid_camera_pause();

        esp_err_t err = partid_config_load(&s_cfg);
        if (err != ESP_OK || !s_cfg.ssid[0]) {
            ESP_LOGW(TAG, "no wifi credentials, see partid_rpc.h");
            finish("Not provisioned", "iris rpc 20481 1");
            continue;
        }

        uint32_t connect_ms = 0;
        set_status("Joining wifi");
        err = partid_net_connect(s_cfg.ssid, s_cfg.pass, WIFI_TIMEOUT_MS, &connect_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi failed 0x%x", (int)err);
            finish("Wifi failed", s_cfg.ssid);
            continue;
        }

        /*
         * An SSID without a key is a useful state, not a broken one. The
         * association is half the cost of a query and it is worth measuring on
         * its own, so the run gets this far, records what the radio spent and
         * stops at the request. The stats below are real for this path.
         */
        if (!s_cfg.api_key[0]) {
            memset(&s_result, 0, sizeof(s_result));
            snprintf(s_result.text, sizeof(s_result.text), "No API key");
            err = ESP_ERR_NOT_FOUND;
        } else {
            set_status(connect_ms ? "Uploading" : "Uploading, wifi was up");
            err = partid_vision_identify(&s_cfg, s_jpeg, size, &s_result);
        }
        const int8_t rssi = partid_net_rssi();

        /* Dropping the station is a setting, because holding it up and
         * re-associating per query are two different costs and the write up
         * quotes both. */
        if (s_cfg.cycle_wifi) {
            (void)partid_net_disconnect();
        }

        /* One line, numbers only. This is the measurement record: grep QUERY
         * out of the Iris log and it is already a table. */
        ESP_LOGI(TAG,
                 "QUERY seq=%u err=0x%x http=%d jpeg=%u sent=%u recv=%u "
                 "encode_us=%u connect_ms=%u request_ms=%u response_ms=%u "
                 "total_ms=%u rssi=%d ptok=%d otok=%d",
                 (unsigned)seq, (int)err, s_result.http_status, (unsigned)size,
                 (unsigned)s_result.sent_bytes, (unsigned)s_result.recv_bytes,
                 (unsigned)encode_us, (unsigned)connect_ms,
                 (unsigned)s_result.request_ms, (unsigned)s_result.response_ms,
                 (unsigned)s_result.total_ms, (int)rssi, s_result.prompt_tokens,
                 s_result.output_tokens);
        partid_rpc_publish(seq, err, &s_result, connect_ms, encode_us, size, rssi);

        refresh_topbar();

        if (err == ESP_OK) {
            finish("Tap for another", s_result.text);
        } else if (err == ESP_ERR_NOT_FOUND) {
            finish("Wifi up, no API key", "iris rpc 20481 1 gkey=");
        } else {
            finish("Failed, tap to retry",
                   s_result.text[0] ? s_result.text : "No answer");
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /*
     * A live preview submits a new cache key every frame, so GSP caches each
     * 460800 byte image and then evicts it, logging one INFO line per frame.
     * At preview rate that is eight lines a second into a 4 kB ESP-Iris log
     * ring, and the device was already reporting dropped log bytes. The QUERY
     * lines this project exists to collect would be competing with it, so the
     * eviction chatter goes to WARN and above.
     */
    esp_log_level_set("gsp_image_cache", ESP_LOG_WARN);

    iris_ota_support_start();

    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x)", bundle_err);
        return;
    }

    esp_display_present_target_config_t display;
    ESP_ERROR_CHECK(board_display_init(&display));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_touch_init(&touch));

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;

    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&app_config, &lcd, &s_ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(s_ui));
    ESP_ERROR_CHECK(esp_gsp_on_event(s_ui, on_touch, NULL));

    s_job = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_job ? ESP_OK : ESP_ERR_NO_MEM);

    /*
     * Control surfaces first, camera second, and the order matters.
     *
     * This used to start the camera first and return early when it failed. A
     * wedged I2C bus on the module then took the RPC service and the settings
     * pages down with it, so the one board that needed remote inspection was
     * the one that could not be reached, and the screen could not be used to
     * fix anything either. A peripheral failing must never cost the operator
     * their way in.
     */
    ESP_ERROR_CHECK(partid_net_init());
    ESP_ERROR_CHECK(partid_ui_init(s_ui));
    ESP_ERROR_CHECK(partid_rpc_register(on_rpc_query));

    /*
     * Retried, because the first attempt races the bus.
     *
     * The module's I2C bus is sometimes still busy when the application
     * starts: SCCB fails, the camera reports not found, and the module
     * manager logs "Module scan recovered: slot=left" in the same
     * millisecond. So the bus comes good a moment after the one attempt that
     * needed it. Four tries a second apart turns a dead camera into a slightly
     * slow start, and a genuinely absent module still only costs three
     * seconds.
     */
    esp_err_t cam_err = ESP_FAIL;
    for (int attempt = 1; attempt <= CAMERA_START_ATTEMPTS; ++attempt) {
        cam_err = partid_camera_start(s_ui, on_capture, NULL);
        if (cam_err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(TAG, "camera started on attempt %d", attempt);
            }
            break;
        }
        ESP_LOGW(TAG, "camera attempt %d failed 0x%x", attempt, (int)cam_err);
        if (attempt < CAMERA_START_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
        }
    }

    if (cam_err != ESP_OK) {
        /* Degraded, not dead. Settings still open and RPC still answers. */
        ESP_LOGE(TAG, "camera unavailable 0x%x", (int)cam_err);
        set_status("No camera. Tap the shutter to retry");
        set_result("Check the LEFT module");
    } else {
        s_jpeg_cap = partid_camera_frame_capacity();
        s_jpeg = heap_caps_malloc(s_jpeg_cap, MALLOC_CAP_SPIRAM);
        ESP_ERROR_CHECK(s_jpeg ? ESP_OK : ESP_ERR_NO_MEM);

        /* Apply the stored view settings before anyone looks at the preview. */
        if (partid_config_load(&s_cfg) == ESP_OK) {
            partid_camera_set_rotation(s_cfg.rotation);
            partid_camera_set_mirror(s_cfg.mirror);
        }
        refresh_flash_button();
    }

    /* Outside the branch on purpose. With it inside, a board whose camera
     * failed showed the scene's placeholder "Not set up" in the top bar even
     * with wifi and a key stored, which made a camera fault look like a
     * configuration fault. */
    refresh_topbar();

    if (xTaskCreate(query_task, "partid_query", QUERY_TASK_STACK, NULL, 5, NULL) !=
        pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    /* Said once at boot, so an unprovisioned board explains itself before
     * anyone taps it. */
    if (partid_config_load(&s_cfg) == ESP_OK && !partid_config_complete(&s_cfg)) {
        set_status("Not provisioned");
        set_result("iris rpc 20481 1");
    }

    ESP_LOGI(TAG, "Part ID ready at %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
}
