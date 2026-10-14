idf_component_register(
    SRCS
        "src/ui_services.c"
        "src/board_display.c"
        "src/factory_ui.c"
        "src/factory_ui_dispatch.c"
        "src/iris_screen_mirror.c"
        "src/vibe_bundle.c"
        "src/vibe_lcd_budget.c"
        "src/vibe_png_inflate.c"
        "src/vibe_transfer.c"
        "src/vibe_ui.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES esp-gsp
    PRIV_REQUIRES app_network app_ideas app_nand_update app_system app_system_update app_bridge esp_iris esp-mosaico-bsp esp_display_present esp_lcd esp_driver_spi esp_timer esp_wifi
)

# GSP's prebuilt package exports its public touch adapter as an INTERFACE
# source. Compile it here once instead of propagating C sources into main.
# Force extraction because the release archive references the adapter later
# in the link. This changes build targets only, never managed source files.
get_property(vibe_gsp_lib GLOBAL PROPERTY ESP_GSP_COMPONENT_LIB)
get_property(vibe_gsp_dir GLOBAL PROPERTY ESP_GSP_COMPONENT_DIR)
get_target_property(vibe_gsp_sources ${vibe_gsp_lib} INTERFACE_SOURCES)
set(vibe_touch_source "${vibe_gsp_dir}/adapters/esp_idf/esp_gsp_idf_touch.c")
if(vibe_touch_source IN_LIST vibe_gsp_sources)
    list(REMOVE_ITEM vibe_gsp_sources "${vibe_touch_source}")
    set_property(TARGET ${vibe_gsp_lib} PROPERTY INTERFACE_SOURCES "${vibe_gsp_sources}")
    target_sources(${COMPONENT_LIB} PRIVATE "${vibe_touch_source}")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,gsp_esp_touch_init")
endif()

# Vibe Mode is self-contained: scenes and subset fonts live in vibe-mode.bin.
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "../../ui/main.json"
    PROFILE "../../ui/profile.yaml"
    PIXEL_FORMAT rgb565
    SYMBOL bundle)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,__wrap_spi_bus_initialize"
    "-Wl,-u,__wrap_esp_lcd_new_panel_io_spi"
    "-Wl,--wrap=spi_bus_initialize"
    "-Wl,--wrap=esp_lcd_new_panel_io_spi"
    "-Wl,--wrap=esp_display_presenter_submit_buffer"
    "-Wl,--wrap=tinfl_decompress")

# Store a compressed copy instead of the helper's uncompressed embed source.
set(vibe_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}/gsp_gen_bundle")
set_source_files_properties("${vibe_bundle_dir}/bundle_embed.S"
    PROPERTIES HEADER_FILE_ONLY TRUE)
idf_build_get_property(vibe_python PYTHON)
set(vibe_packed "${CMAKE_CURRENT_BINARY_DIR}/vibe_bundle_data.c")
add_custom_command(OUTPUT "${vibe_packed}"
    COMMAND "${vibe_python}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_vibe_bundle.py"
        "${vibe_bundle_dir}/bundle.gspb" "${vibe_packed}"
    DEPENDS "${vibe_bundle_dir}/bundle.gspb"
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_vibe_bundle.py"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${vibe_packed}")
