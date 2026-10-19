idf_component_register(
    SRCS
        "src/bridge_boot.c"
        "src/bridge_control.c"
        "src/bridge_execute.c"
        "src/bridge_http.c"
        "src/bridge_session.c"
        "src/bridge_validation.c"
        "src/factory_bridge.c"
        "src/iris_bridge.c"
        "src/system_plan.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES app_network app_http_client app_system app_system_update esp_mosaico_boot efuse esp_http_client esp-tls espressif__cjson mbedtls esp_app_format esp_hw_support esp_partition spi_flash app_update bootloader_support esp_iris nvs_flash esp_timer esp_wifi
)
