idf_component_register(
    SRCS
        "src/app_storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp-mosaico-bsp esp_lcd_touch esp_blockdev spi_nand_flash joltwallet__littlefs esp_driver_spi esp_driver_gpio
)
