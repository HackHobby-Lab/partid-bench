// SPDX-License-Identifier: Apache-2.0
//
// One JPEG out, one answer back, and the numbers the answer cost.
//
// The body is written to the socket in pieces rather than assembled first. A
// 480 square JPEG is 20 to 40 kB, which is 27 to 54 kB once base64 expands it,
// and holding that twice to no purpose would be careless on a board that has
// to run off 80 mAh. Streaming also means the byte count below is what the
// radio actually sent, not an estimate.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "partid_config.h"

typedef struct {
    char text[192];       /* the answer, markdown stripped and trimmed */
    int http_status;
    uint32_t request_ms;  /* socket open, TLS handshake and the body out */
    uint32_t response_ms; /* body out finished to body in complete */
    uint32_t total_ms;
    size_t sent_bytes;    /* request body only, what the radio paid for */
    size_t recv_bytes;
    int prompt_tokens;    /* from usageMetadata, for the cost per query line */
    int output_tokens;
} partid_vision_result_t;

/**
 * @brief POST one JPEG and read back the identification. Blocking, seconds.
 *
 * Returns ESP_OK only when the model actually answered. On any other outcome
 * @p out still carries the timings and byte counts, because a failed query
 * costs the battery exactly as much as a successful one.
 */
esp_err_t partid_vision_identify(const partid_config_t *cfg,
                                 const uint8_t *jpeg, size_t jpeg_size,
                                 partid_vision_result_t *out);
