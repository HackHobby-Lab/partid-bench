// SPDX-License-Identifier: Apache-2.0

#include "partid_config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define PARTID_NVS_NAMESPACE "partid"

static const char *TAG = "partid_cfg";

/*
 * Not gemini-2.5-flash, which is what every tutorial and every older example
 * names. A new API key gets 404 NOT_FOUND from it:
 *
 *   This model models/gemini-2.5-flash is no longer available to new users.
 *
 * It still appears in the models list with generateContent among its supported
 * methods, so listing the models does not reveal the problem. Only calling it
 * does. Overridable with `model=` over RPC, which is the only reason this cost
 * one round trip instead of a rebuild.
 */
static const char DEFAULT_MODEL[] = "gemini-3.8-flash";

/*
 * Two short lines out, nothing else, and the break between them matters.
 *
 * The panel draws line 1 in the big accent row and line 2 in the small row
 * under it, so the model putting both on one line costs the part number its
 * row. main.c can rescue that case and does, but asking plainly for two lines
 * is cheaper than guessing where to cut one.
 *
 * The rows are 432 px wide: roughly 24 characters at font 26 for the part
 * number, and two lines of about 36 at font 16 for the description. Six words
 * fits. A chatty answer does not fit the screen at all.
 */
static const char DEFAULT_PROMPT[] =
    "This photo shows one electronic component. Reply on exactly two lines. "
    "Line 1, its part number exactly as marked. Line 2, at most six words "
    "saying what it is. No other text and no markdown. If no marking is "
    "legible reply UNREADABLE.";

/* One table, so load and set cannot drift apart. */
typedef struct {
    const char *key;
    size_t offset;
    size_t cap;
} field_t;

static const field_t FIELDS[] = {
    { "ssid",   offsetof(partid_config_t, ssid),    PARTID_SSID_CAP },
    { "pass",   offsetof(partid_config_t, pass),    PARTID_PASS_CAP },
    { "gkey",   offsetof(partid_config_t, api_key), PARTID_KEY_CAP },
    { "model",  offsetof(partid_config_t, model),   PARTID_MODEL_CAP },
    { "prompt", offsetof(partid_config_t, prompt),  PARTID_PROMPT_CAP },
};

#define FIELD_COUNT (sizeof(FIELDS) / sizeof(FIELDS[0]))

esp_err_t partid_config_load(partid_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->model, sizeof(out->model), "%s", DEFAULT_MODEL);
    snprintf(out->prompt, sizeof(out->prompt), "%s", DEFAULT_PROMPT);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARTID_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* never provisioned; the defaults stand */
    }
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Size first, then read straight into the destination slot. No temporary
     * copy means nothing large lands on the caller's stack, and a slot is only
     * written once a usable value is known to exist, so the defaults above
     * survive a missing or oversized key untouched.
     */
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        size_t len = 0;
        if (nvs_get_str(nvs, FIELDS[i].key, NULL, &len) != ESP_OK) {
            continue;
        }
        if (len <= 1 || len > FIELDS[i].cap) {
            continue; /* empty, or written by an older and wider build */
        }
        (void)nvs_get_str(nvs, FIELDS[i].key, (char *)out + FIELDS[i].offset, &len);
    }

    uint8_t cycle = 0;
    if (nvs_get_u8(nvs, "cycle", &cycle) != ESP_OK) {
        cycle = 0;
    }
    out->cycle_wifi = cycle != 0;

    /* Stored in quarter turns so it fits a u8 and cannot hold a value the PPA
     * has no enum for. */
    uint8_t quarters = PARTID_DEFAULT_ROTATION / 90U;
    if (nvs_get_u8(nvs, "rot", &quarters) != ESP_OK || quarters > 3) {
        quarters = PARTID_DEFAULT_ROTATION / 90U;
    }
    out->rotation = (uint16_t)(quarters * 90U);

    uint8_t flip = 0;
    if (nvs_get_u8(nvs, "flip", &flip) != ESP_OK) {
        flip = 0;
    }
    out->mirror = flip != 0;

    nvs_close(nvs);
    return ESP_OK;
}

bool partid_config_complete(const partid_config_t *cfg)
{
    return cfg && cfg->ssid[0] && cfg->api_key[0];
}

esp_err_t partid_config_set(const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARTID_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (strcmp(key, "flip") == 0) {
        const bool on = value[0] == '1' || value[0] == 'y' || value[0] == 'Y';
        err = nvs_set_u8(nvs, "flip", on ? 1 : 0);
    } else if (strcmp(key, "rot") == 0) {
        const int degrees = atoi(value);
        if (degrees % 90 != 0 || degrees < 0 || degrees > 270) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = nvs_set_u8(nvs, "rot", (uint8_t)(degrees / 90));
        }
    } else if (strcmp(key, "cycle") == 0) {
        const bool on = value[0] == '1' || value[0] == 'y' || value[0] == 'Y';
        err = nvs_set_u8(nvs, "cycle", on ? 1 : 0);
    } else {
        err = ESP_ERR_NOT_FOUND;
        for (size_t i = 0; i < FIELD_COUNT; ++i) {
            if (strcmp(key, FIELDS[i].key) != 0) {
                continue;
            }
            err = strlen(value) >= FIELDS[i].cap ? ESP_ERR_INVALID_SIZE
                                                 : nvs_set_str(nvs, FIELDS[i].key, value);
            break;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    /* The key name and the length, never the value. These logs get pasted
     * into the write up. */
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stored %s, %u bytes", key, (unsigned)strlen(value));
    } else {
        ESP_LOGW(TAG, "rejected %s (0x%x)", key, (int)err);
    }
    return err;
}

void partid_config_describe(const partid_config_t *cfg, char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    if (!cfg) {
        snprintf(out, cap, "no config");
        return;
    }
    snprintf(out, cap, "ssid=%s pass=%s gkey=%s model=%s prompt=%ub cycle=%d rot=%u flip=%d",
             cfg->ssid[0] ? cfg->ssid : "unset",
             cfg->pass[0] ? "set" : "unset",
             cfg->api_key[0] ? "set" : "unset",
             cfg->model,
             (unsigned)strlen(cfg->prompt),
             cfg->cycle_wifi ? 1 : 0, (unsigned)cfg->rotation, cfg->mirror ? 1 : 0);
}
