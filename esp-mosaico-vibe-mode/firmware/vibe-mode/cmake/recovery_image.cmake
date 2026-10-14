idf_build_get_property(recovery_idf_target IDF_TARGET)
idf_build_get_property(recovery_python PYTHON)

if(NOT CONFIG_FACTORY_RECOVERY_FIRMWARE)
    message(FATAL_ERROR "firmware/vibe-mode must build the retained Vibe Mode firmware")
endif()
if(NOT CONFIG_ESP_IRIS_OTA)
    message(FATAL_ERROR "The retained Recovery firmware requires the ESP-Iris OTA writer")
endif()

partition_table_get_partition_info(
    recovery_partition_offset "--partition-name vibe_mode" "offset")
partition_table_get_partition_info(
    recovery_partition_size "--partition-name vibe_mode" "size")
partition_table_get_partition_info(
    normal_partition_offset "--partition-name main_app" "offset")
partition_table_get_partition_info(
    ota_data_partition_offset "--partition-name otadata" "offset")

if(NOT recovery_partition_offset OR NOT recovery_partition_size)
    message(FATAL_ERROR "The partition table must contain the Recovery slot")
endif()
if(NOT normal_partition_offset OR NOT ota_data_partition_offset)
    message(FATAL_ERROR "The partition table does not support the retained Recovery workflow")
endif()

set(recovery_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_recovery.py")
set(recovery_output_dir "${CMAKE_BINARY_DIR}/recovery")
set(recovery_current_output_dir "${CMAKE_BINARY_DIR}/recovery-current")
set(recovery_source_description "${CMAKE_BINARY_DIR}/project_description.json")
set(recovery_source_bootloader "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin")
set(recovery_source_partition_table
    "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin")
set(recovery_source_ota_data "${CMAKE_BINARY_DIR}/recovery-ota-data.bin")
add_custom_command(OUTPUT "${recovery_source_ota_data}"
    COMMAND "${recovery_python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/boot_intent.py"
        "${recovery_source_ota_data}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../tools/boot_intent.py"
    VERBATIM)
add_custom_target(recovery-boot-intent DEPENDS "${recovery_source_ota_data}")
set(recovery_source_application "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin")
# ESP-IDF accepts an app if it fits any app partition; ota_0 is larger than
# vibe_mode here. Make overflow of the retained slot a hard build failure.
add_custom_target(recovery-slot-check ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DRECOVERY_BINARY=${recovery_source_application}"
        "-DRECOVERY_SLOT_SIZE=${recovery_partition_size}"
        -P "${CMAKE_CURRENT_LIST_DIR}/check_recovery_slot.cmake"
    COMMENT "Checking the immutable Recovery slot size"
    VERBATIM)
add_dependencies(recovery-slot-check app)
# This project builds Vibe Mode itself. IDF's ordinary application default is
# main_app; replace its generated entries so native flash cannot put this
# maintenance image in the user's application slot. Product applications keep
# the standard IDF targets and never include this file.
foreach(native_target flash app-flash)
    set_property(TARGET ${native_target} PROPERTY IMAGES "")
    set_property(TARGET ${native_target} PROPERTY FLASH_FILE "")
    set_property(TARGET ${native_target} PROPERTY FLASH_ENTRY "")
    if(TARGET encrypted-${native_target})
        foreach(image_property FLASH_FILE FLASH_ENTRY ENCRYPTED_IMAGES NON_ENCRYPTED_IMAGES)
            set_property(TARGET encrypted-${native_target} PROPERTY ${image_property} "")
        endforeach()
        add_dependencies(encrypted-${native_target} recovery-slot-check)
    endif()
    esptool_py_flash_target_image(${native_target} app
        "${recovery_partition_offset}" "${recovery_source_application}")
    add_dependencies(${native_target} recovery-slot-check)
endforeach()
esptool_py_flash_target_image(flash bootloader
    "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}" "${recovery_source_bootloader}")
esptool_py_flash_target_image(flash partition-table
    "${CONFIG_PARTITION_TABLE_OFFSET}" "${recovery_source_partition_table}")
esptool_py_flash_target_image(flash otadata
    "${ota_data_partition_offset}" "${recovery_source_ota_data}")
add_dependencies(flash recovery-boot-intent)
if(TARGET encrypted-flash)
    add_dependencies(encrypted-flash recovery-boot-intent)
endif()
set(recovery_self_update_component_dir
    "${CMAKE_BINARY_DIR}/recovery-self-update-components")
set(recovery_self_update_manifest
    "${CMAKE_BINARY_DIR}/recovery-self-update-manifest.json")
set(recovery_self_update_bundle
    "${CMAKE_BINARY_DIR}/${PROJECT_NAME}-recovery-update.irisfw")
set(recovery_bundle_tool
    "${CMAKE_CURRENT_LIST_DIR}/../../../../ESP-Iris/components/esp_iris/tools/system_update_bundle.py")

# Reviewed images keep their original identity until a new bundle is validated.
# Read the application filename so the same targets also work after publication
# of a reviewed vibe-mode build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RECOVERY_PREBUILT_DIR}/manifest.json")
file(READ "${RECOVERY_PREBUILT_DIR}/manifest.json" recovery_prebuilt_manifest)
string(JSON recovery_prebuilt_application GET "${recovery_prebuilt_manifest}"
    images recovery file)
set(recovery_bundle_common_files
    bootloader.bin partition-table.bin ota_data_initial.bin manifest.json)
set(recovery_reviewed_bundle_files
    ${recovery_bundle_common_files} "${recovery_prebuilt_application}")
set(recovery_current_bundle_files
    ${recovery_bundle_common_files} "${PROJECT_NAME}.bin")

if(CONFIG_SECURE_BOOT)
    set(recovery_secure_boot 1)
else()
    set(recovery_secure_boot 0)
endif()
if(CONFIG_SECURE_FLASH_ENC_ENABLED)
    set(recovery_flash_encryption 1)
else()
    set(recovery_flash_encryption 0)
endif()

set(recovery_layout_args
    --target "${recovery_idf_target}"
    --bootloader-offset "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
    --partition-table-offset "${CONFIG_PARTITION_TABLE_OFFSET}"
    --ota-data-offset "${ota_data_partition_offset}"
    --recovery-offset "${recovery_partition_offset}"
    --recovery-size "${recovery_partition_size}"
    --normal-offset "${normal_partition_offset}"
    --secure-boot "${recovery_secure_boot}"
    --flash-encryption "${recovery_flash_encryption}")

set(recovery_prebuilt_inputs
    --bootloader "${RECOVERY_PREBUILT_DIR}/bootloader.bin"
    --partition-table "${RECOVERY_PREBUILT_DIR}/partition-table.bin"
    --ota-data "${RECOVERY_PREBUILT_DIR}/ota_data_initial.bin"
    --recovery "${RECOVERY_PREBUILT_DIR}/${recovery_prebuilt_application}"
    --manifest "${RECOVERY_PREBUILT_DIR}/manifest.json")

set(recovery_source_inputs
    --bootloader "${recovery_source_bootloader}"
    --partition-table "${recovery_source_partition_table}"
    --ota-data "${recovery_source_ota_data}"
    --recovery "${recovery_source_application}"
    --project-description "${recovery_source_description}"
    --source-root "${CMAKE_CURRENT_LIST_DIR}/..")

set(recovery_prebuilt_dependencies "${recovery_tool}")
foreach(bundle_file IN LISTS recovery_reviewed_bundle_files)
    list(APPEND recovery_prebuilt_dependencies "${RECOVERY_PREBUILT_DIR}/${bundle_file}")
endforeach()

set(recovery_output_files "")
set(recovery_current_output_files "")
foreach(bundle_file IN LISTS recovery_reviewed_bundle_files)
    list(APPEND recovery_output_files "${recovery_output_dir}/${bundle_file}")
endforeach()
foreach(bundle_file IN LISTS recovery_current_bundle_files)
    list(APPEND recovery_current_output_files "${recovery_current_output_dir}/${bundle_file}")
endforeach()

add_custom_command(
    OUTPUT ${recovery_output_files}
    COMMAND "${recovery_python}" "${recovery_tool}"
        ${recovery_prebuilt_inputs}
        ${recovery_layout_args}
        --output-dir "${recovery_output_dir}"
    DEPENDS ${recovery_prebuilt_dependencies}
    COMMENT "Validating the reviewed Recovery bundle"
    VERBATIM)
add_custom_target(recovery-image DEPENDS ${recovery_output_files})

add_custom_target(recovery-current-bundle
    COMMAND "${recovery_python}" "${recovery_tool}"
        ${recovery_source_inputs}
        ${recovery_layout_args}
        --output-dir "${recovery_current_output_dir}"
    BYPRODUCTS ${recovery_current_output_files}
    DEPENDS "${recovery_tool}"
    COMMENT "Packaging an unreviewed Recovery candidate from the current build"
    VERBATIM)
add_dependencies(recovery-current-bundle app bootloader partition_table_bin recovery-boot-intent)

if(NOT CONFIG_SPIRAM_XIP_FROM_PSRAM)
    message(FATAL_ERROR
        "Recovery self-update requires CONFIG_SPIRAM_XIP_FROM_PSRAM")
endif()
if(NOT CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
    message(FATAL_ERROR
        "Recovery self-update bundle currently requires the 16 MiB product layout")
endif()

math(EXPR recovery_partition_offset_decimal
    "${recovery_partition_offset}" OUTPUT_FORMAT DECIMAL)
math(EXPR recovery_partition_size_decimal
    "${recovery_partition_size}" OUTPUT_FORMAT DECIMAL)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../recovery-self-update-manifest.template.json.in"
    "${recovery_self_update_manifest}"
    @ONLY)

# This target produces a single-component Recovery system-update archive. The
# partition table is hashed as an explicit layout precondition but is never
# included as a component. The builder pads vibe-mode.bin to the complete vibe_mode
# slot so the device can verify one deterministic protected range from PSRAM.
add_custom_target(recovery-self-update-bundle
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${recovery_self_update_component_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${recovery_source_application}"
        "${recovery_self_update_component_dir}/${PROJECT_NAME}.bin"
    COMMAND "${recovery_python}" "${recovery_bundle_tool}"
        "${recovery_self_update_manifest}"
        --component-root "${recovery_self_update_component_dir}"
        --target-layout "${recovery_source_partition_table}"
        --output "${recovery_self_update_bundle}"
    BYPRODUCTS "${recovery_self_update_bundle}"
    DEPENDS "${recovery_bundle_tool}" "${recovery_source_partition_table}"
        "${CMAKE_CURRENT_LIST_DIR}/../recovery-self-update-manifest.template.json.in"
    COMMENT "Packaging the current Recovery as a self-update bundle"
    VERBATIM)
add_dependencies(recovery-self-update-bundle app partition_table_bin)

add_custom_target(update-recovery-prebuilt
    COMMAND "${recovery_python}" "${recovery_tool}"
        ${recovery_source_inputs}
        ${recovery_layout_args}
        --output-dir "${RECOVERY_PREBUILT_DIR}"
    DEPENDS "${recovery_tool}"
    COMMENT "Atomically publishing the complete reviewed Recovery bundle"
    VERBATIM)
add_dependencies(update-recovery-prebuilt app bootloader partition_table_bin recovery-boot-intent)

set(MOSAICO_RECOVERY_SOURCE "reviewed" CACHE STRING
    "Internal source used by mosaico-recover-flash: reviewed or current")
set_property(CACHE MOSAICO_RECOVERY_SOURCE PROPERTY STRINGS reviewed current)
if(MOSAICO_RECOVERY_SOURCE STREQUAL "reviewed")
    set(mosaico_recovery_bundle_dir "${recovery_output_dir}")
    set(mosaico_recovery_bundle_target recovery-image)
    set(mosaico_recovery_application "${recovery_prebuilt_application}")
elseif(MOSAICO_RECOVERY_SOURCE STREQUAL "current")
    set(mosaico_recovery_bundle_dir "${recovery_current_output_dir}")
    set(mosaico_recovery_bundle_target recovery-current-bundle)
    set(mosaico_recovery_application "${PROJECT_NAME}.bin")
    message(WARNING "mosaico-recover-flash will use an unreviewed current-source bundle")
else()
    message(FATAL_ERROR "MOSAICO_RECOVERY_SOURCE must be reviewed or current")
endif()

# Prepare and verify every artifact before a device maintenance lease is
# acquired. The flash target depends on the same bundle and does not compile
# while the USB endpoint is detached.
add_custom_target(mosaico-recover-prepare
    DEPENDS ${mosaico_recovery_bundle_target})

# This is the only low-level write target used by mosaico.py. ESP-IDF owns the
# flasher arguments; the Python product CLI never assembles an esptool command.
esptool_py_custom_target(
    mosaico-recover-flash mosaico_recover ${mosaico_recovery_bundle_target})
esptool_py_flash_target_image(
    mosaico-recover-flash recovery_bootloader
    "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
    "${mosaico_recovery_bundle_dir}/bootloader.bin")
esptool_py_flash_target_image(
    mosaico-recover-flash recovery_partition_table
    "${CONFIG_PARTITION_TABLE_OFFSET}"
    "${mosaico_recovery_bundle_dir}/partition-table.bin")
esptool_py_flash_to_partition(
    mosaico-recover-flash otadata
    "${mosaico_recovery_bundle_dir}/ota_data_initial.bin")
esptool_py_flash_to_partition(
    mosaico-recover-flash vibe_mode
    "${mosaico_recovery_bundle_dir}/${mosaico_recovery_application}")

message(STATUS
    "Recovery bundle source for mosaico-recover-flash: ${MOSAICO_RECOVERY_SOURCE}")
message(STATUS
    "Recovery self-update bundle: ${recovery_self_update_bundle}")
