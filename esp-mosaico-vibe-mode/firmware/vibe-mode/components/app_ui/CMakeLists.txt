idf_component_register(
    SRCS
        "src/ui_services.c"
        "src/board_display.c"
        "src/factory_ui.c"
        "src/factory_ui_dispatch.c"
        "src/iris_screen_mirror.c"
        "src/vibe_lcd_budget.c"
        "src/vibe_transfer.c"
        "src/vibe_ui.c"
        "src/vibe_home_motion.c"
        "src/vibe_motion.c"
        "src/vibe_product.c"
        "light/vui.c"
        "light/vui_decode.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES app_settings app_device
    PRIV_REQUIRES app_update_check app_network app_ideas app_nand_update app_system app_system_update app_bridge esp_iris esp-mosaico-bsp esp_display_present esp_lcd esp_lcd_touch esp_driver_spi esp_timer esp_wifi esp_app_format
    EMBED_FILES "light/vui_assets.bin"
)
# Raster loops run for every scroll frame. Keep the product's size-oriented
# defaults elsewhere, but allow inlining and loop optimization in the renderer.
set_source_files_properties("light/vui.c" PROPERTIES COMPILE_OPTIONS "-O2")
# The presenter is a transport driver. Vibe owns layout, drawing and input.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_display_presenter_submit_buffer")

idf_build_get_property(vui_python PYTHON)
execute_process(COMMAND "${vui_python}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/check_vui_assets.py"
    COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(vui-assets-check
    COMMAND "${vui_python}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/check_vui_assets.py"
    COMMENT "Checking lightweight UI resource provenance"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} vui-assets-check)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,__wrap_spi_bus_initialize"
    "-Wl,-u,__wrap_esp_lcd_new_panel_io_spi"
    "-Wl,--wrap=spi_bus_initialize"
    "-Wl,--wrap=esp_lcd_new_panel_io_spi")
