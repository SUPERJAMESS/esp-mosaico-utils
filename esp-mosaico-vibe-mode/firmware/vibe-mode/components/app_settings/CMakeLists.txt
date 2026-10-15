idf_component_register(
    SRCS "src/vibe_settings.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_system app_network app_storage app_system_update esp_mosaico_boot nvs_flash esp_system
)
