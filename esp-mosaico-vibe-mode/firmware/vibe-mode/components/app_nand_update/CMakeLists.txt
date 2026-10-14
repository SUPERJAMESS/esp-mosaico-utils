idf_component_register(
    SRCS
        "src/factory_nand_update.c"
    INCLUDE_DIRS "include"
    REQUIRES app_system_update
    PRIV_REQUIRES app_storage app_system esp_mosaico_boot esp_iris espressif__cjson mbedtls
)
