idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES app_system app_settings app_device app_storage app_network app_system_update app_nand_update
        app_ideas app_bridge app_ui app_iris esp_iris esp_mosaico_boot nvs_flash
)
