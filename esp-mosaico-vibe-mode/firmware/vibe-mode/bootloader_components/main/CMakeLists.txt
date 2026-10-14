idf_component_register(
    SRCS
        "bootloader_start.c"
        "mosaico_boot_nvs.c"
        "mosaico_boot_splash.c"
    PRIV_INCLUDE_DIRS "../../../../include"
    REQUIRES
        bootloader
        bootloader_support
        efuse
        esp_hal_gpio
        esp_hal_gpspi
        esp_hal_usb
        esp_rom
        hal
        soc
        nvs_flash
)

idf_component_get_property(nvs_source nvs_flash COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${nvs_source}/private_include")

# The retained bootloader has an immutable 24 KiB window.  Prefer minimum
# code size for the custom partition selector and LCD splash paths.
target_compile_options(${COMPONENT_LIB} PRIVATE -Oz)
foreach(size_component bootloader_support nvs_flash nvs_sec_provider)
    idf_component_get_property(size_lib ${size_component} COMPONENT_LIB)
    target_compile_options(${size_lib} PRIVATE -Oz)
endforeach()

# Keep the default ESP-IDF bootloader linker layout.
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
