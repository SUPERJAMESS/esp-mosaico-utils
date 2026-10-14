idf_component_register(
    SRCS
        "src/factory_ideas.c"
        "src/ideas_cloud.c"
        "src/ideas_font.c"
        "src/ideas_integrity.c"
        "src/ideas_image.c"
        "src/ideas_title.c"
        "src/ideas_library.c"
        "src/ideas_package.c"
        "src/ideas_presentation.c"
        "src/ideas_preview.c"
        "src/ideas_store.c"
        "src/ideas_worker.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES app_network app_storage app_nand_update app_system app_system_update esp_http_client esp-tls esp_timer spi_flash app_update esp_app_format nvs_flash espressif__cjson mbedtls
)

if(CONFIG_IRIS_FACTORY_IDEAS_CA_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IDEAS_PRIVATE_CA=1)
    configure_file("${CONFIG_IRIS_FACTORY_IDEAS_CA_FILE}" "${CMAKE_CURRENT_BINARY_DIR}/ideas_ca.pem" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/ideas_ca.pem" TEXT)
endif()
