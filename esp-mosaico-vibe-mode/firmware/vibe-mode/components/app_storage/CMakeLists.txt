idf_component_register(
    SRCS
        "src/app_storage.c"
        "src/storage_metrics.c"
        "src/storage_blockdev.c"
        "src/storage_volume.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES esp-mosaico-bsp esp_lcd_touch esp_blockdev spi_nand_flash joltwallet__littlefs esp_driver_spi esp_driver_gpio esp_timer
)
if(CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE)
    # Observe only this NAND's SPI handle. Count physical reads from commands:
    # LTO can inline the NAND driver's private nand_read entry.
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=spi_device_transmit")
endif()
