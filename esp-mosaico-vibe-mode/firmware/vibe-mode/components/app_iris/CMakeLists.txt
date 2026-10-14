idf_component_register(
    SRCS
        "src/app_iris_storage.c"
        "src/factory_recovery_control.c"
        "src/factory_system_inventory.c"
        "src/factory_ui_input.c"
        "src/recovery_ota_support.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_system app_storage app_network app_nand_update app_system_update app_bridge esp_iris esp_app_format app_update esp_partition esp_timer espressif__cjson nvs_flash mbedtls spi_flash
)
