// SPDX-License-Identifier: Apache-2.0
//
// Host entry point for the GSP simulator.
//
// There is no camera on the host, so the simulator renders the scene with its
// placeholder image. It is still useful for checking the overlay bar, the text
// positions and the touch target.

#include "gsp_sim_bridge.h"

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    (void)ui;
    return ESP_GSP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui) { (void)ui; }
