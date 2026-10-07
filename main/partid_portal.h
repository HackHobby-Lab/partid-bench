// SPDX-License-Identifier: Apache-2.0
//
// A setup portal, served from the board's own access point.
//
// The on screen keyboard can enter everything this does, so why both. Because
// an API key is 39 characters of mixed case and typing that on a touchscreen
// once is tolerable and twice is not. A phone keyboard, and paste, are simply
// better for long secrets. The on device pages stay the answer when there is
// no phone to hand.
//
// It runs only while asked for. The access point and the HTTP server together
// are the largest block of internal RAM this application ever holds, and
// internal RAM is the resource that already broke the camera once, so nothing
// here is started at boot.

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** SSID of the setup network, filled with the device specific name. */
esp_err_t partid_portal_ssid(char *out, size_t cap);

/** Password for the setup network. Derived per device, shown on screen. */
esp_err_t partid_portal_password(char *out, size_t cap);

/** Bring up the access point and the web server. Idempotent. */
esp_err_t partid_portal_start(void);

/** Tear both down and give the memory back. Idempotent. */
esp_err_t partid_portal_stop(void);

bool partid_portal_is_running(void);
