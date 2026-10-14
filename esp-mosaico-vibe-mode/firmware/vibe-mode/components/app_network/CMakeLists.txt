idf_component_register(
    SRCS
        "src/factory_network.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_system esp_wifi esp_event esp_netif esp_timer nvs_flash mdns esp_iris
)
