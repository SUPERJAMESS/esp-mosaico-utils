idf_component_register(SRCS "app_vibe_settings.c" "app_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-mosaico-bsp esp_mosaico_app_recovery nvs_flash esp_wifi
        esp_event esp_netif freertos esp_timer log)
