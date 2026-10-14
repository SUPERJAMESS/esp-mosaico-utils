idf_component_register(
    SRCS
        "src/factory_system_update.c"
        "src/update_flash.c"
        "src/update_iris_adapter.c"
        "src/update_manifest.c"
        "src/update_policy.c"
        "src/update_transaction.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES esp_iris espressif__cjson
    PRIV_REQUIRES app_system esp_mosaico_boot app_update bootloader_support esp_app_format esp_partition spi_flash mbedtls
)
