// SPDX-License-Identifier: Apache-2.0
//
// Station wifi, on demand.
//
// The BSP has mosaico_network and an example called wifi_http_cam, and neither
// is this. mosaico_network is the ESP-NOW mesh that joins modules to each
// other. wifi_http_cam is a SoftAP serving frames to a phone. A station that
// reaches the public internet is not covered by either, so it is written out
// here in plain ESP-IDF.
//
// Connect is separate from init on purpose. The radio costs current the moment
// it associates, and the whole point of this project is to know how much, so
// the association has to be something the application starts and stops rather
// than something that happens at boot.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Bring up the station interface without associating. Safe to call twice. */
esp_err_t partid_net_init(void);

/**
 * @brief Associate and wait for an address.
 *
 * @param out_ms  Time from the first call to a usable address, milliseconds.
 *                Zero when the station was already up. This is the figure the
 *                measurement piece needs isolated from the request itself.
 */
esp_err_t partid_net_connect(const char *ssid, const char *pass,
                             uint32_t timeout_ms, uint32_t *out_ms);

/**
 * @brief Start the wifi driver without associating.
 *
 * Separate from connecting because the access point and a scan both need the
 * driver running while the station stays idle.
 */
esp_err_t partid_net_ensure_started(void);

/** One access point from a scan. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} partid_net_ap_t;

/**
 * @brief Scan for access points, strongest first, one entry per name.
 *
 * Blocking for a couple of seconds, so never call it from an event callback.
 * Hidden networks report an empty name and are dropped, because there is
 * nothing useful to show the user for them.
 */
esp_err_t partid_net_scan(partid_net_ap_t *out, size_t max, size_t *out_count);

/** Drop the association. The radio stays initialised, so reconnect is quick. */
esp_err_t partid_net_disconnect(void);

bool partid_net_is_up(void);

/** Signal strength of the current AP in dBm, or 0 when not associated. */
int8_t partid_net_rssi(void);
