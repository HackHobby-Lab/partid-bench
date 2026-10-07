// SPDX-License-Identifier: Apache-2.0
//
// Credentials and query settings, held in NVS, provisioned over ESP-Iris RPC.
//
// Nothing secret is compiled in. This firmware image is meant to be
// publishable, so the wifi password and the API key reach the device at
// runtime and live in the app-owned `nvs` partition. ESP-Iris keeps its own
// state in `sysmeta` (CONFIG_ESP_IRIS_NVS_PARTITION_NAME), so the two never
// collide and provisioning survives an app update.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PARTID_SSID_CAP   33
#define PARTID_PASS_CAP   65
#define PARTID_KEY_CAP    129
#define PARTID_MODEL_CAP  49
#define PARTID_PROMPT_CAP 321

/*
 * The sensor is mounted rotated on this board. The BSP preview example uses
 * 270, which leaves the image upside down: measured against the model, a
 * module marked ESP32-C3-MINI-1 came back as "E2635-C-WM-I" at 270 and the
 * screenshot showed the text inverted. 90 is the upright value.
 */
#define PARTID_DEFAULT_ROTATION 90

typedef struct {
    char ssid[PARTID_SSID_CAP];
    char pass[PARTID_PASS_CAP];
    char api_key[PARTID_KEY_CAP];
    char model[PARTID_MODEL_CAP];
    char prompt[PARTID_PROMPT_CAP];
    /* Drop the station after every query instead of holding it up. The two
     * modes cost very different amounts per query and both numbers are
     * wanted, so this is a setting rather than a decision. */
    bool cycle_wifi;
    /* Degrees the PPA rotates the sensor frame by, one of 0, 90, 180, 270.
     * A setting rather than a constant because what the model can read depends
     * on it, and proving which value is right takes one screenshot per value
     * rather than one rebuild per value. */
    uint16_t rotation;
    /* Mirror the preview, for when the board is held facing the user. */
    bool mirror;
} partid_config_t;

/** Load the stored settings. Model and prompt fall back to defaults. */
esp_err_t partid_config_load(partid_config_t *out);

/** True when an SSID and an API key are both present. */
bool partid_config_complete(const partid_config_t *cfg);

/** Store one `key` as `value`. Unknown keys and overlong values are rejected. */
esp_err_t partid_config_set(const char *key, const char *value);

/** One line of what is set, with the secrets reduced to set or unset. */
void partid_config_describe(const partid_config_t *cfg, char *out, size_t cap);
