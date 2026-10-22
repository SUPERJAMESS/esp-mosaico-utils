idf_component_register(
    SRCS "src/vibe_device.c" "src/vibe_battery.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES app_settings app_network esp-mosaico-bsp esp_lcd_touch esp_timer esp_system esp_driver_gpio esp_driver_i2c tinyusb esp_tinyusb
)
