idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_hello app_vibe_settings esp-mosaico-bsp esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_bundle esp_mosaico_gsp_iris esp_timer
        nvs_flash log)
