// SPDX-License-Identifier: Apache-2.0
//
// Pipeline, and why it is shaped this way.
//
//   SC101IOT ──UYVY──> PPA ──RGB565──> HW JPEG ──> GSP preview
//                       │                 │
//                  crop + scale           └──────> upload (phase 2)
//
// The BSP docs, camera_photo_app and wifi_http_cam are all written around an
// OV3640, which emits JPEG itself. This board carries an SC101IOT (PID 0xDA4A),
// which cannot: asking for JPEG gets "format=JPEG is not supported" from the
// DVP layer and the camera never opens. So the frame is converted and encoded
// on-chip instead. Both the ESP32-S31 PPA and its JPEG encoder are hardware, so
// this costs almost nothing in CPU.
//
// Scaling in the PPA pass is not incidental. Gemini charges a flat ~1089 tokens
// for an image at any resolution, so capture size buys nothing in tokens but
// costs real bytes on the wire and therefore real radio time. Being able to
// pick the capture size at runtime is how that gets measured.

#include "partid_camera.h"

#include <string.h>

/* esp_gsp_set_image_ex() takes a raw bind id and those macros sit behind this
 * guard. The generated wrapper takes no options, and the per-frame cache_key is
 * not optional: without it the decoder reuses the previous frame's pixels. */
#define GSP_BUNDLE_ENABLE_RAW_IDS 1

#include "bundle_gsp.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_jpeg_enc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"

static const char *TAG = "partid_cam";

#define PREVIEW_PX 480  /* square, matches the display */
#define JPEG_QUALITY 80

static struct {
    esp_gsp_handle_t ui;
    mosaico_camera_handle_t camera;
    ppa_client_handle_t ppa;
    jpeg_enc_handle_t jpeg;

    uint16_t *rgb;       /* PPA output, PREVIEW_PX square */
    size_t rgb_bytes;    /* allocated capacity, may exceed the image */
    size_t rgb_image_bytes; /* what the image actually occupies */
    uint8_t *out;        /* encoded JPEG */
    size_t out_cap;

    partid_capture_cb_t on_capture;
    void *ctx;
    volatile bool want_capture;
    /*
     * `streaming` gates the loop and the loop clears it itself after a
     * capture, so a capture is already quiet before the worker calls pause.
     *
     * `warmup` drops the first frames after a wake. The sensor's exposure
     * control needs a few frames to settle, and without this the capture can
     * be served a frame that was already in flight when it went to sleep.
     */
    volatile bool streaming;
    volatile bool parked;
    volatile int warmup;
    volatile ppa_srm_rotation_angle_t rotation;
    volatile bool mirror;
    bool flash_on;
    volatile size_t last_bytes;
    volatile uint32_t last_encode_us;
    uint32_t seq;
    bool decoder_warned;
} s;

static uint32_t align_down_even(uint32_t v) { return v & ~1U; }

/*
 * Container sniffing is independent of the linked decoders, so a JPEG submitted
 * to a build with no JPEG decoder still returns OK and fails only here. Without
 * this the screen stays black with no error anywhere.
 */
static void on_image_complete(esp_gsp_handle_t ui, uint16_t bind,
                              gsp_err_t status, void *complete_ctx)
{
    (void)ui;
    (void)bind;
    (void)complete_ctx;
    if (status == GSP_OK || status == GSP_ERR_CANCELLED) {
        return;
    }
    if (!s.decoder_warned) {
        s.decoder_warned = true;
        ESP_LOGE(TAG, "GSP refused the frame (0x%x); UNSUPPORTED means no JPEG decoder linked",
                 (int)status);
    }
}

/* UYVY -> centre-cropped square RGB565, upright. */
static esp_err_t convert(const mosaico_camera_frame_t *frame)
{
    if (frame->pixel_format != V4L2_PIX_FMT_UYVY) {
        ESP_LOGE(TAG, "unexpected camera format 0x%08x", (unsigned)frame->pixel_format);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /*
     * The PPA scales in 1/16 steps and TRUNCATES, so an unrepresentable factor
     * silently produces a smaller image than the output buffer, leaving the
     * remainder at whatever the buffer held. A full-height centre crop of a
     * 720-row sensor wants 480/720 = 0.667, which truncates to 10/16 = 0.625
     * and writes only 450 columns: a black band down the right edge.
     *
     * So pick the LARGEST centre crop whose scale lands exactly on a step.
     * For 480 out of a 720-high sensor that is a 640 crop at 12/16 = 0.75.
     */
    const uint32_t max_crop = align_down_even(
        frame->width < frame->height ? frame->width : frame->height);
    if (!max_crop) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t crop = 0;
    for (uint32_t frac = 1; frac <= 16; ++frac) {
        if ((PREVIEW_PX * 16U) % frac) {
            continue; /* non-integer crop */
        }
        const uint32_t candidate = (PREVIEW_PX * 16U) / frac;
        if (candidate <= max_crop && candidate > crop) {
            crop = candidate;
        }
    }
    if (!crop) {
        crop = max_crop; /* sensor smaller than the target; accept the truncation */
    }
    /* Stride, not width: the DVP layer pads lines. */
    const uint32_t stride = frame->bytes_per_line ? frame->bytes_per_line
                                                  : frame->width * 2U;

    ESP_RETURN_ON_ERROR(
        esp_cache_msync((void *)frame->data, frame->size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate frame cache");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(s.rgb, s.rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "clean rgb cache");

    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame->data,
            .pic_w = stride / 2U,
            .pic_h = frame->height,
            .block_w = crop,
            .block_h = crop,
            .block_offset_x = align_down_even((frame->width - crop) / 2U),
            .block_offset_y = align_down_even((frame->height - crop) / 2U),
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s.rgb,
            .buffer_size = s.rgb_bytes,
            .pic_w = PREVIEW_PX,
            .pic_h = PREVIEW_PX,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        /*
         * Runtime, not a constant. The sensor is mounted rotated and what the
         * model can read depends entirely on getting this right: at 270, the
         * BSP example's value, a module marked ESP32-C3-MINI-1 came back as
         * "E2635-C-WM-I".
         *
         * No mirroring by default.
         *
         * This previously read mirror_y = !mirror, mirror_x = mirror, copied
         * from the BSP preview example on the assumption that the sensor's
         * mounting needed undoing. It does not, and that form has no
         * un-mirrored state at all: one axis or the other was always flipped,
         * so every frame left here reversed. On screen it is easy to miss,
         * because a mirrored PCB still looks like a PCB. To a model reading
         * the silkscreen it is fatal, and it produced the same garbled part
         * numbers that a wrong rotation does.
         *
         * So the default mirrors nothing, and the flip button is a real
         * toggle for holding the board facing the user.
         */
        .rotation_angle = s.rotation,
        .scale_x = (float)PREVIEW_PX / (float)crop,
        .scale_y = (float)PREVIEW_PX / (float)crop,
        .mirror_x = s.mirror,
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    static bool logged;
    if (!logged) {
        logged = true;
        ESP_LOGI(TAG, "crop %ux%u of %ux%u, scale %.4f -> %dx%d",
                 (unsigned)crop, (unsigned)crop, (unsigned)frame->width,
                 (unsigned)frame->height, (double)PREVIEW_PX / (double)crop,
                 PREVIEW_PX, PREVIEW_PX);
    }
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(s.ppa, &op), TAG, "ppa convert");
    return esp_cache_msync(s.rgb, s.rgb_bytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

/*
 * Software JPEG, not the hardware engine.
 *
 * jpeg_encoder_process() from esp_driver_jpeg hangs the board: it and the PPA
 * and GSP are all 2D DMA clients, and when the encoder tries to reset a channel
 * another client has not released, dma2d_hal_rx_reset_channel() busy-waits with
 * no timeout until the interrupt watchdog fires. That race is latent and became
 * reproducible once wifi came up. Reverting this file to the exact revision
 * that previously survived did NOT help, which is what proved the contention
 * rather than the code. The software encoder touches no DMA at all, so the race
 * cannot occur. It costs CPU, which a few frames per second can afford.
 */
static esp_err_t encode(size_t *out_size)
{
    int written = 0;
    const int64_t t0 = esp_timer_get_time();
    const jpeg_error_t err = jpeg_enc_process(s.jpeg, (const uint8_t *)s.rgb,
                                              (int)s.rgb_image_bytes, s.out,
                                              (int)s.out_cap, &written);
    s.last_encode_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg encode failed %d", (int)err);
        return ESP_FAIL;
    }
    *out_size = (size_t)written;
    return ESP_OK;
}

static void preview_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s.streaming) {
            /* Parked, holding no camera buffer. There are only two, so
             * sitting on one while the radio works would starve the DVP. */
            s.parked = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s.parked = false;

        mosaico_camera_frame_t frame = {0};
        esp_err_t err = mosaico_camera_get_frame(s.camera, &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "frame error 0x%x, restarting", err);
            if (mosaico_camera_restart(s.camera) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        size_t jpeg_size = 0;
        err = convert(&frame);
        if (err == ESP_OK) {
            /* No yield here. A vTaskDelay() between the PPA pass and the
             * encode was tried and made things WORSE: it hands the scheduler a
             * window in which GSP's render task can take the 2D DMA engine, so
             * the encoder then collides with GSP instead of following the PPA
             * cleanly. Keep this tight. */
            err = encode(&jpeg_size);
        }
        /* Return the camera buffer as early as possible: there are only two. */
        (void)mosaico_camera_return_frame(s.camera, &frame);

        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s.last_bytes = jpeg_size;

        /* Submitted before the capture callback, so the frame left on screen
         * is the frame that was sent away, not the one before it. */
        const esp_gsp_image_options_t options = {
            .ownership = ESP_GSP_IMAGE_COPY,
            .on_complete = on_image_complete,
            .cache_key = ++s.seq ? s.seq : 1, /* 0 means legacy caching */
        };
        (void)esp_gsp_set_image_ex(s.ui, GSP_PARTID_BIND_PREVIEW, s.out, jpeg_size,
                                   &options);

        /* Settling after a wake. The frame is shown but not handed over. */
        if (s.warmup > 0) {
            --s.warmup;
            continue;
        }

        if (s.want_capture) {
            s.want_capture = false;
            if (s.on_capture) {
                s.on_capture(s.out, jpeg_size, PREVIEW_PX, PREVIEW_PX, s.ctx);
            }
            /* The capture is the last frame anyone needs until the result is
             * dismissed. Parking from inside the loop means the pipeline is
             * already quiet by the time the query starts. */
            s.streaming = false;
        }
    }
}

esp_err_t partid_camera_start(esp_gsp_handle_t ui, partid_capture_cb_t on_capture,
                              void *ctx)
{
    if (!ui) {
        return ESP_ERR_INVALID_ARG;
    }
    s.ui = ui;
    s.on_capture = on_capture;
    s.ctx = ctx;
    s.streaming = true;
    s.rotation = PPA_SRM_ROTATION_ANGLE_90;

    /*
     * Allocated by hand, deliberately, after measuring.
     *
     * jpeg_alloc_encoder_mem() looks like the correct API and reports the same
     * 460800/230400 sizes, but with it the first jpeg_encoder_process() hangs
     * forever inside dma2d_hal_rx_reset_channel()'s untimed spin and the
     * interrupt watchdog resets the board. This form ran for over 450 seconds
     * and many captures. Do not "fix" it back without testing on hardware.
     */
    s.rgb_bytes = (size_t)PREVIEW_PX * PREVIEW_PX * 2U;
    s.rgb_image_bytes = s.rgb_bytes;
    s.out_cap = s.rgb_bytes / 2U;
    s.rgb = heap_caps_aligned_calloc(64, 1, s.rgb_bytes, MALLOC_CAP_SPIRAM);
    s.out = heap_caps_aligned_calloc(64, 1, s.out_cap, MALLOC_CAP_SPIRAM);
    if (!s.rgb || !s.out) {
        ESP_LOGE(TAG, "encoder buffers unavailable");
        return ESP_ERR_NO_MEM;
    }

    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s.ppa), TAG, "ppa client");

    jpeg_enc_config_t jpeg_cfg = {
        .width = PREVIEW_PX,
        .height = PREVIEW_PX,
        /* The PPA writes little-endian RGB565. If red and blue come out
         * swapped, this is the field to change, not the PPA. */
        .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = JPEG_QUALITY,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
        .hfm_task_priority = 0,
        .hfm_task_core = 0,
    };
    if (jpeg_enc_open(&jpeg_cfg, &s.jpeg) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "software jpeg encoder open failed");
        return ESP_FAIL;
    }

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    /* UYVY, not JPEG. The SC101IOT on this board cannot produce JPEG. */
    config.pixel_format = MOSAICO_CAMERA_PIXEL_FORMAT_UYVY;
    config.allow_unidentified = true;

    esp_err_t err = mosaico_camera_new(&config, &s.camera);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera not found (0x%x). Is it in the LEFT slot?", err);
        return err;
    }
    if ((err = mosaico_camera_open(s.camera)) != ESP_OK) {
        ESP_LOGE(TAG, "camera open failed 0x%x", err);
        return err;
    }
    if ((err = mosaico_camera_start_stream(s.camera)) != ESP_OK) {
        ESP_LOGE(TAG, "camera stream failed 0x%x", err);
        return err;
    }

    mosaico_camera_info_t info = {0};
    if (mosaico_camera_get_info(s.camera, &info) == ESP_OK) {
        ESP_LOGI(TAG, "sensor %ux%u stride %u, %u buffers -> %dx%d JPEG q%d",
                 (unsigned)info.width, (unsigned)info.height,
                 (unsigned)info.bytes_per_line, (unsigned)info.buffer_count,
                 PREVIEW_PX, PREVIEW_PX, JPEG_QUALITY);
    }

    if (xTaskCreate(preview_task, "partid_cam", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t partid_camera_set_flash(bool on)
{
    if (!s.camera) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == s.flash_on) {
        return ESP_OK;
    }
    const esp_err_t err = on ? mosaico_camera_flash_trigger(s.camera)
                             : mosaico_camera_flash_stop(s.camera);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flash %s failed 0x%x", on ? "on" : "off", err);
        return err;
    }
    s.flash_on = on;
    return ESP_OK;
}

bool partid_camera_flash_is_on(void) { return s.flash_on; }

void partid_camera_set_mirror(bool on) { s.mirror = on; }

bool partid_camera_mirror_is_on(void) { return s.mirror; }

void partid_camera_set_rotation(uint16_t degrees)
{
    switch (degrees) {
    case 0:   s.rotation = PPA_SRM_ROTATION_ANGLE_0;   break;
    case 90:  s.rotation = PPA_SRM_ROTATION_ANGLE_90;  break;
    case 180: s.rotation = PPA_SRM_ROTATION_ANGLE_180; break;
    case 270: s.rotation = PPA_SRM_ROTATION_ANGLE_270; break;
    default:
        ESP_LOGW(TAG, "ignoring rotation %u", (unsigned)degrees);
        return;
    }
    ESP_LOGI(TAG, "rotation now %u", (unsigned)degrees);
}

void partid_camera_request_capture(void) { s.want_capture = true; }

esp_err_t partid_camera_pause(void)
{
    if (!s.camera) {
        return ESP_ERR_INVALID_STATE;
    }
    s.streaming = false;

    /*
     * Wait for the loop to confirm it has stopped. get_frame has its own
     * timeout, so this cannot block forever.
     *
     * The sensor deliberately keeps streaming. Both ways of stopping it,
     * mosaico_camera_stop_stream() and mosaico_camera_set_power_down(true),
     * free the DVP controller, and coming back allocates the DVP backup
     * buffer again. Once the wifi stack holds its share of internal RAM that
     * allocation fails:
     *
     *   esp_cam_new_dvp_ctlr: no mem for DVP backup buffer
     *   VIDIOC_STREAMON failed: errno=12 (Not enough space)
     *
     * On a board that also uses wifi, stopping the camera is a one way door.
     * It survived three cycles here and failed on the fourth. So the pipeline
     * parks instead: no convert, no encode, no GSP submit. That removes the
     * 119 ms of software JPEG per frame, which is the cost that would
     * otherwise dominate a query measurement. What remains is the sensor and
     * the DVP idling at a constant rate, which belongs in a baseline run and
     * gets subtracted.
     */
    for (int i = 0; i < 100 && !s.parked; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s.parked) {
        ESP_LOGW(TAG, "preview loop did not park");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t partid_camera_resume(void)
{
    if (!s.camera) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.streaming) {
        /* Buffers filled while parked are stale by definition. */
        s.warmup = 3;
    }
    s.streaming = true;
    return ESP_OK;
}

size_t partid_camera_frame_capacity(void) { return s.out_cap; }

size_t partid_camera_last_frame_bytes(void) { return s.last_bytes; }

uint32_t partid_camera_last_encode_us(void) { return s.last_encode_us; }
