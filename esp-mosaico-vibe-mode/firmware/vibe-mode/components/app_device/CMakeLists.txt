idf_component_register(
    SRCS "src/vibe_device.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_settings app_network esp-mosaico-bsp esp_lcd_touch esp_timer esp_driver_gpio tinyusb esp_tinyusb
)
