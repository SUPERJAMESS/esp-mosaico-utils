# Build one Raylib Lite Engine native game as a managed ESP-Iris application.
# Include from a project CMakeLists.txt before project():
#
#   cmake_minimum_required(VERSION 3.16)
#   set(RAYLIB_LITE_GAME_DIR /path/to/my_game)
#   include(/path/to/mosaico-tools/cmake/raylib_lite_iris_app.cmake)
#   project(my_game VERSION 1.0.0)
#   raylib_lite_iris_link_game_board()
#   include("${MOSAICO_SYSTEM_UPDATE_CMAKE}")
#
# The project directory must contain templates/blank_game/partitions.csv.
# `mosaico.py game build --target iris <game>` generates such a project.
# RAYLIB_LITE_ENGINE_ROOT and MOSAICO_BSP_ROOT are CMake or environment inputs.
include_guard(GLOBAL)

get_filename_component(_raylib_iris_tools "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_raylib_iris_utils "${_raylib_iris_tools}/.." ABSOLUTE)
set(_raylib_iris_template "${_raylib_iris_tools}/templates/raylib_lite_iris")
set(_raylib_iris_product "${_raylib_iris_tools}/templates/blank_game")

if(NOT RAYLIB_LITE_ENGINE_ROOT AND DEFINED ENV{RAYLIB_LITE_ENGINE_ROOT})
    set(RAYLIB_LITE_ENGINE_ROOT "$ENV{RAYLIB_LITE_ENGINE_ROOT}")
endif()
if(NOT EXISTS "${RAYLIB_LITE_ENGINE_ROOT}/include/raylib_lite/raylib_lite_native_services.h")
    message(FATAL_ERROR
        "RAYLIB_LITE_ENGINE_ROOT must name a Raylib Lite Engine checkout with "
        "native application services: '${RAYLIB_LITE_ENGINE_ROOT}'")
endif()
get_filename_component(RAYLIB_LITE_ENGINE_ROOT "${RAYLIB_LITE_ENGINE_ROOT}" ABSOLUTE)

if(NOT RAYLIB_LITE_GAME_DIR)
    message(FATAL_ERROR "Set RAYLIB_LITE_GAME_DIR to a native game project directory")
endif()
get_filename_component(_raylib_iris_game "${RAYLIB_LITE_GAME_DIR}" ABSOLUTE)
if(NOT EXISTS "${_raylib_iris_game}/main/CMakeLists.txt")
    message(FATAL_ERROR "Game project needs main/CMakeLists.txt: ${_raylib_iris_game}")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
    message(FATAL_ERROR "Copy ${_raylib_iris_product}/partitions.csv into ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
# Reuse the Engine's application-side Board and common native launcher.
# The product owns required Iris services; the Board supplies hardware only.
set(SDKCONFIG_DEFAULTS
    "${_raylib_iris_product}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.game.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.game.defaults")
endif()
if(EXISTS "${_raylib_iris_game}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${_raylib_iris_game}/sdkconfig.defaults")
endif()
if(EXISTS "${_raylib_iris_game}/sdkconfig.application.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${_raylib_iris_game}/sdkconfig.application.defaults")
endif()
list(APPEND SDKCONFIG_DEFAULTS
    "${_raylib_iris_product}/sdkconfig.application.defaults"
    "${_raylib_iris_template}/sdkconfig.application.defaults")
# Keep Recovery and Iris on the consuming workspace's revision.
set(ESP_IRIS_BUILD_PROFILE "usb" CACHE STRING "Iris game USB profile" FORCE)
set(RAYLIB_LITE_NATIVE_SERVICE_COMPONENT esp_mosaico_raylib_iris)
list(APPEND EXTRA_COMPONENT_DIRS
    "${_raylib_iris_tools}/components/esp_mosaico_raylib_iris"
    "${_raylib_iris_utils}/ESP-Iris/components/esp_iris"
    "${_raylib_iris_utils}/esp-mosaico-vibe-mode/components/esp_mosaico_app_recovery")
if(NOT RAYLIB_LITE_BSP_DIR AND DEFINED ENV{MOSAICO_BSP_ROOT})
    set(RAYLIB_LITE_BSP_DIR "$ENV{MOSAICO_BSP_ROOT}")
endif()
include("${RAYLIB_LITE_ENGINE_ROOT}/examples/common_components/examples_common/project.cmake")
list(APPEND EXTRA_COMPONENT_DIRS "${_raylib_iris_game}/main")
if(EXISTS "${_raylib_iris_game}/components")
    list(APPEND EXTRA_COMPONENT_DIRS "${_raylib_iris_game}/components")
endif()
set(MOSAICO_ESP_IRIS_ROOT "${_raylib_iris_utils}/ESP-Iris")
set(MOSAICO_SYSTEM_UPDATE_CMAKE "${_raylib_iris_tools}/cmake/system_update.cmake")
include("${_raylib_iris_utils}/esp-mosaico-vibe-mode/cmake/mosaico_idf_project.cmake")

# An external Game main component does not receive IDF's local-main implicit
# dependencies. Preserve the explicit selected-Board link for its audio calls.
function(raylib_lite_iris_link_game_board)
    idf_component_get_property(_game_lib main COMPONENT_LIB)
    idf_component_get_property(_board_lib "${RAYLIB_LITE_BOARD}" COMPONENT_LIB)
    target_link_libraries(${_game_lib} PUBLIC ${_board_lib})
endfunction()
