# The Vibe Mode DMA buffers remain in internal SRAM, but are zeroed by IDF's
# startup instead of occupying Flash. Use TinyUSB's configuration hook so the
# managed sources and the adapter's remaining configuration stay untouched.
idf_build_get_property(vibe_usb_components BUILD_COMPONENTS)
set(vibe_tinyusb_component "")
foreach(candidate espressif__tinyusb tinyusb)
    if(candidate IN_LIST vibe_usb_components)
        set(vibe_tinyusb_component "${candidate}")
        break()
    endif()
endforeach()
if(NOT vibe_tinyusb_component)
    message(FATAL_ERROR "Vibe Mode requires TinyUSB for the Recovery USB link")
endif()
idf_component_get_property(vibe_tinyusb_lib ${vibe_tinyusb_component} COMPONENT_LIB)
target_compile_definitions(${vibe_tinyusb_lib} PRIVATE
    CFG_TUSB_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/tusb_config_vibe.h")

# Check the actual linked result, including LTO, instead of relying only on the
# source attribute. A dependency/linker update must not move DMA into PSRAM or
# move these zero-filled objects back into the on-disk image.
idf_build_get_property(vibe_storage_python PYTHON)
add_custom_target(runtime-storage-check ALL
    COMMAND "${vibe_storage_python}"
        "${CMAKE_CURRENT_LIST_DIR}/../tools/check_runtime_storage.py"
        "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf"
        "${CMAKE_BINARY_DIR}/config/sdkconfig.json"
    COMMENT "Checking internal startup-cleared USB and Iris storage"
    VERBATIM)
add_dependencies(runtime-storage-check app)
