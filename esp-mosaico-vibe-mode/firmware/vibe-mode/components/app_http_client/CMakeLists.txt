idf_component_register(
    SRCS "src/factory_http_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client
    PRIV_REQUIRES esp_iris
)
