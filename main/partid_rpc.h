// SPDX-License-Identifier: Apache-2.0
//
// The provisioning and measurement surface, over ESP-Iris RPC.
//
// Secrets reach the device this way rather than through a compiled constant or
// a captive portal, because ESP-Iris is already the transport the toolchain
// uses and it already owns High Speed USB. From the host:
//
//   .\m.ps1 iris rpc 20481 1 --payload "ssid=MyNetwork"
//   .\m.ps1 iris rpc 20481 1 --payload "pass=..."
//   .\m.ps1 iris rpc 20481 1 --payload "gkey=..."
//   .\m.ps1 iris rpc 20481 2
//   .\m.ps1 iris rpc 20481 3 --deadline-ms 20000
//   .\m.ps1 iris rpc 20481 4
//   .\m.ps1 iris rpc 20481 5 --payload "settings"
//
// Method 3 fires a query with no hand on the board and method 4 reads the
// numbers back as JSON, which together are what makes an unattended run of a
// hundred queries possible. A measurement taken while someone taps a
// touchscreen is a measurement of tapping a touchscreen.

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "partid_vision.h"

/* 0x5001, clear of ESP-Iris's own 0x1001 pointer and 0x7FFF recovery IDs.
 * The CLI takes decimal, which is 20481. */
#define PARTID_RPC_SERVICE_ID 0x5001U
#define PARTID_RPC_METHOD_SET    1U
#define PARTID_RPC_METHOD_STATUS 2U
#define PARTID_RPC_METHOD_QUERY  3U
#define PARTID_RPC_METHOD_STATS  4U
/* Navigate the UI without touching the panel. Exists so screens can be
 * captured and a demo can be replayed exactly, not as a debug leftover. */
#define PARTID_RPC_METHOD_PAGE   5U
/* Toggle the setup portal, so it can be verified and demonstrated without a
 * hand on the panel. */
#define PARTID_RPC_METHOD_PORTAL 6U

/** Called on the ESP-Iris service task. Must return promptly. */
typedef esp_err_t (*partid_rpc_trigger_fn)(void);

esp_err_t partid_rpc_register(partid_rpc_trigger_fn trigger);

/** Publish one finished query for method 4 to read back. */
void partid_rpc_publish(uint32_t seq, esp_err_t status,
                        const partid_vision_result_t *result,
                        uint32_t connect_ms, uint32_t encode_us,
                        size_t jpeg_bytes, int8_t rssi);
