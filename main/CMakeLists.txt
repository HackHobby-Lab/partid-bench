idf_component_register(
    SRCS "partid_rpc.c" "partid_vision.c" "partid_net.c" "partid_config.c" "partid_portal.c" "partid_ui.c" "main.c" "partid_camera.c" "board_display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp mosaico_module_camera esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd
        esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls espressif__cjson)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
