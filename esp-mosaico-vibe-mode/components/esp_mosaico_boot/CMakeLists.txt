idf_component_register(
    SRCS "mosaico_boot.c"
    INCLUDE_DIRS "include" "../../include"
    REQUIRES app_update bootloader_support esp_iris esp_partition nvs_flash
)
