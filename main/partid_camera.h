// SPDX-License-Identifier: Apache-2.0
//
// Camera to screen, and camera to cloud, from the same bytes.
//
// The OV3640 emits JPEG itself and GSP accepts a runtime encoded JPEG on a
// resource bind, so a frame goes from sensor to display with no decode, no
// colour conversion and no PPA pass in the application at all. The identical
// buffer is what gets uploaded, so a capture costs nothing extra.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called once per capture, from the preview task.
 *
 * @param jpeg  Encoded frame. Valid only for the duration of the call; copy it
 *              if the consumer outlives the callback.
 */
typedef void (*partid_capture_cb_t)(const uint8_t *jpeg, size_t size,
                                    uint32_t width, uint32_t height, void *ctx);

/** Start the camera and the preview task feeding the GSP preview bind. */
esp_err_t partid_camera_start(esp_gsp_handle_t ui, partid_capture_cb_t on_capture,
                              void *ctx);

/**
 * @brief Request that the next frame be delivered to the capture callback.
 *
 * The pipeline pauses itself once that frame has been handed over, so the
 * captured image stays on screen and nothing is running behind it.
 */
void partid_camera_request_capture(void);

/**
 * @brief Stop the convert and encode loop.
 *
 * Not a display freeze. GSP holds the last frame submitted to it, so the
 * screen keeps the capture for free, and this stops the work behind it: no
 * PPA pass, no software JPEG, no submit. That matters because the whole
 * project is a measurement of what one cloud query costs, and 119 ms of
 * software JPEG per frame running behind the query would land in the trace as
 * though the radio had spent it.
 *
 * The sensor keeps streaming. Stopping it cannot be undone once wifi holds its
 * share of internal RAM; see the note in the implementation. So a trace still
 * carries a constant sensor and DVP idle, which a baseline run subtracts.
 *
 * Idempotent. Blocks until the loop has parked.
 */
esp_err_t partid_camera_pause(void);

/** Restart the loop, dropping the frames that went stale while parked. */
esp_err_t partid_camera_resume(void);

/**
 * @brief Set the rotation applied in the PPA pass, 0, 90, 180 or 270 degrees.
 *
 * Takes effect on the next frame. Anything else is ignored.
 */
void partid_camera_set_rotation(uint16_t degrees);

/**
 * @brief Turn the camera module's flash LED on or off, as a torch.
 *
 * Not a strobe. A bench tool reads fine print off a dark board, so the light
 * stays on while the user frames the shot rather than firing at capture.
 */
esp_err_t partid_camera_set_flash(bool on);
bool partid_camera_flash_is_on(void);

/** Mirror the image across the axis the user sees as horizontal. */
void partid_camera_set_mirror(bool on);
bool partid_camera_mirror_is_on(void);

/** Largest frame the encoder can produce, so a caller can size a copy once. */
size_t partid_camera_frame_capacity(void);

/** Most recent encoded frame size in bytes, for logging and measurement. */
size_t partid_camera_last_frame_bytes(void);

/** Software JPEG encode time for the last frame, microseconds. */
uint32_t partid_camera_last_encode_us(void);

#ifdef __cplusplus
}
#endif
