cmake_minimum_required(VERSION 3.16)

# `mosaico.py recover` passes this standard ESP-IDF variable with `-D`.
# Accepting the same name from the environment also keeps low-noise and
# standalone build runners independent of workspace-specific variable names.
if(NOT DEFINED EXTRA_COMPONENT_DIRS AND DEFINED ENV{EXTRA_COMPONENT_DIRS})
    set(EXTRA_COMPONENT_DIRS "$ENV{EXTRA_COMPONENT_DIRS}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/esp_mosaico_boot")

set(RECOVERY_PREBUILT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/prebuilt/recovery" CACHE PATH
    "Directory containing the checked-in recovery image and manifest")

if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "sdkconfig.defaults;sdkconfig.recovery.defaults")
endif()

# Use the same standalone compiler bootstrap as generated applications. Without
# this, ESP-GSP falls back to importing an unprovisioned Python gsp module.
# Recovery retains GSP 1.4.0 independently of the application template pins.
set(MOSAICO_GSPC_VERSION "0.5.0")
include("${CMAKE_CURRENT_LIST_DIR}/../../../mosaico-tools/cmake/gsp_compiler.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vibe-mode)

if(NOT CONFIG_ESPTOOLPY_FLASHMODE_QIO)
    message(FATAL_ERROR "Vibe Mode requires CONFIG_ESPTOOLPY_FLASHMODE_QIO=y; update sdkconfig and rebuild")
endif()

include(cmake/recovery_image.cmake)
