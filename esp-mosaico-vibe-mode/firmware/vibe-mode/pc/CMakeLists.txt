cmake_minimum_required(VERSION 3.20)
project(vibe_simulator LANGUAGES C)
find_package(ZLIB REQUIRED)
set(ui "${CMAKE_CURRENT_LIST_DIR}/../components/app_ui")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/check_vui_assets.py"
    COMMAND_ERROR_IS_FATAL ANY)
set(asset_c "${CMAKE_CURRENT_BINARY_DIR}/assets.c")
add_custom_command(OUTPUT "${asset_c}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/embed_assets.py" "${ui}/light/vui_assets.bin" "${asset_c}"
    DEPENDS "${ui}/light/vui_assets.bin" "${CMAKE_CURRENT_LIST_DIR}/embed_assets.py"
    VERBATIM)
add_library(vibe_ui SHARED platform_pc.c ../components/app_ideas/src/ideas_title.c ${ui}/src/vibe_ui.c ${ui}/src/vibe_product.c
    ${ui}/src/vibe_home_motion.c
    ${ui}/src/vibe_motion.c
    ${ui}/src/vibe_transfer.c ${ui}/light/vui.c ${ui}/light/vui_decode.c
    "${asset_c}")
target_include_directories(vibe_ui PRIVATE ${ui}/include ${ui}/private ${ui}/light
    ../components/app_ideas/include ../components/app_settings/include ../components/app_device/include)
target_link_libraries(vibe_ui PRIVATE ZLIB::ZLIB)
if(NOT MSVC)
    target_link_libraries(vibe_ui PRIVATE m)
endif()
if(MSVC)
    target_compile_options(vibe_ui PRIVATE /W4)
else()
    target_compile_options(vibe_ui PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set_target_properties(vibe_ui PROPERTIES C_STANDARD 11 WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(VUI_TEST_MAIN)
    add_executable(vui_runtime_test "${VUI_TEST_MAIN}")
    target_include_directories(vui_runtime_test PRIVATE ${ui}/include ${ui}/private ../components/app_ideas/include)
    target_link_libraries(vui_runtime_test PRIVATE vibe_ui)
endif()

add_custom_target(vui_assets_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/check_vui_assets.py"
    VERBATIM)
add_dependencies(vibe_ui vui_assets_check)
