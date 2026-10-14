# Optional hardware-only component. Add explicitly via EXTRA_COMPONENT_DIRS;
# the normal firmware build never includes or registers this probe.
idf_component_register(SRCS "probe.c" "runtime.c" PRIV_REQUIRES app_storage app_ui esp-gsp esp_timer mbedtls spi_flash)
idf_component_get_property(probe_ui_lib app_ui COMPONENT_LIB)
idf_component_get_property(probe_ui_dir app_ui COMPONENT_DIR)
target_link_libraries(${probe_ui_lib} INTERFACE ${COMPONENT_LIB})
get_target_property(probe_ui_includes ${probe_ui_lib} INCLUDE_DIRECTORIES)
target_include_directories(${COMPONENT_LIB} PRIVATE ${probe_ui_includes})
# Capture the UI through its normal attach call without adding a product RPC or
# changing managed components. Restrict the macro to this one translation unit.
set_source_files_properties("${probe_ui_dir}/src/factory_ui.c" TARGET_DIRECTORY ${probe_ui_lib}
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/attach.h")

if(CONFIG_NAND_IDEAS_DRAM_STACK OR CONFIG_NAND_IDEAS_RTC_STACK)
    idf_component_get_property(probe_ideas_lib app_ideas COMPONENT_LIB)
    idf_component_get_property(probe_ideas_dir app_ideas COMPONENT_DIR)
    set_source_files_properties("${probe_ideas_dir}/src/ideas_worker.c" TARGET_DIRECTORY ${probe_ideas_lib}
        PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/ideas_stack.h")
endif()
