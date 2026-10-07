// SPDX-License-Identifier: Apache-2.0
//
// The settings, wifi and text entry pages.
//
// Split out of main.c because the scan page and the setup pages have almost
// nothing in common: one is a camera, the other is a form. main.c keeps the
// camera and hands everything else here.
//
// All four pages live in one GSP scene as full-screen containers whose
// visibility is bound, so switching page is one call and no scene reload.

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_gsp.h"

typedef enum {
    PARTID_PAGE_SCAN = 0,
    PARTID_PAGE_SETTINGS,
    PARTID_PAGE_WIFI,
    PARTID_PAGE_TEXT,
} partid_page_t;

/** Start the setup pages. Spawns the worker that scans, so call it once. */
esp_err_t partid_ui_init(esp_gsp_handle_t ui);

/**
 * @brief Offer a touch event to the setup pages.
 *
 * @return true when it belonged here and the caller should stop. The scan
 *         page's own buttons are left for main.c.
 */
bool partid_ui_handle_event(const esp_gsp_event_t *event);

partid_page_t partid_ui_page(void);

/** Switch page and refresh whatever that page shows. */
void partid_ui_show(partid_page_t page);
