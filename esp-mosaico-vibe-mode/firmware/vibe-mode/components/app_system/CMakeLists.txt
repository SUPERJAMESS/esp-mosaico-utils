idf_component_register(
    SRCS
        "src/app_system_startup.c"
        "src/factory_system_metadata.c"
        "src/factory_locale.c"
    INCLUDE_DIRS "include" "../../../../include"
    # Product ABI remains owned by esp-mosaico-vibe-mode/include.
    REQUIRES esp_iris esp_timer
    PRIV_REQUIRES nvs_flash esp_mosaico_boot esp_driver_gpio esp_app_format app_update esp-mosaico-bsp esp_lcd_touch
)
