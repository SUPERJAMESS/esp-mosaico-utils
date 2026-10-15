idf_component_register(
    SRCS "src/factory_update_check.c" "src/factory_firmware_install.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bootloader_support app_system_update esp_iris app_update esp_partition spi_flash esp_hw_support esp_http_client esp_app_format esp_timer mbedtls espressif__cjson freertos
)
