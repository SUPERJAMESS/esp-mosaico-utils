idf_component_register(
    SRCS
        "src/factory_ideas.c"
        "src/ideas_actions.c"
        "src/ideas_development.c"
        "src/ideas_cloud.c"
        "src/ideas_font.c"
        "src/ideas_library.c"
        "src/ideas_package.c"
        "src/ideas_presentation.c"
        "src/ideas_preview.c"
        "src/ideas_store.c"
        "src/ideas_worker.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES app_settings app_network app_storage app_nand_update app_system app_system_update esp_iris esp_http_client esp-tls esp_timer spi_flash app_update bootloader_support esp_app_format esp_mosaico_boot nvs_flash espressif__cjson mbedtls
)
