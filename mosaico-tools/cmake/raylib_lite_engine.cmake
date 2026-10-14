# Product integration for the single-component Raylib Lite Engine.
# GSP compiler discovery stays here; the engine no longer ships cmake/mosaico_game_sdk.cmake.
if(NOT EXISTS "${RAYLIB_LITE_ENGINE_ROOT}/idf_component.yml" OR
        NOT EXISTS "${RAYLIB_LITE_ENGINE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Set RAYLIB_LITE_ENGINE_ROOT to the initialized Raylib Lite Engine checkout")
endif()

set(MOSAICO_GAME_GSPC_FETCHER "${CMAKE_CURRENT_LIST_DIR}/../tools/gsp-sim/fetch_gspc.py")
set(MOSAICO_GAME_RECOVERY_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../esp-mosaico-vibe-mode/components/esp_mosaico_app_recovery")

function(mosaico_game_sdk_configure_gsp_compiler)
    # IDF 6.2's GCC 16 diagnoses a bounded strncpy in managed mdns 1.13.0 as
    # stringop-truncation. Keep all other default warnings fatal while the
    # upstream managed component catches up with the toolchain.
    add_compile_options(-Wno-error=stringop-truncation)
    if(DEFINED GSPC_EXECUTABLE OR DEFINED ENV{GSPC_EXECUTABLE})
        return()
    endif()
    find_program(_mosaico_python NAMES python3 python)
    set(_mosaico_fetch_gspc "${MOSAICO_GAME_GSPC_FETCHER}")
    if(_mosaico_python AND EXISTS "${_mosaico_fetch_gspc}")
        execute_process(
            COMMAND "${_mosaico_python}" "${_mosaico_fetch_gspc}"
            OUTPUT_VARIABLE _mosaico_cached_gspc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _mosaico_gspc_result)
        if(_mosaico_gspc_result EQUAL 0 AND EXISTS "${_mosaico_cached_gspc}")
            set(GSPC_EXECUTABLE "${_mosaico_cached_gspc}" CACHE FILEPATH
                "Standalone ESP-GSP scene compiler")
            return()
        endif()
    endif()
    find_program(_mosaico_gspc NAMES gspc gspc-dev)
    if(_mosaico_gspc)
        set(GSPC_EXECUTABLE "${_mosaico_gspc}" CACHE FILEPATH
            "ESP-GSP scene compiler")
    endif()
endfunction()

function(mosaico_raylib_lite_add_engine)
    list(APPEND EXTRA_COMPONENT_DIRS "${RAYLIB_LITE_ENGINE_ROOT}")
    if(MOSAICO_GAME_RECOVERY_COMPONENT_DIR)
        if(NOT EXISTS "${MOSAICO_GAME_RECOVERY_COMPONENT_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "MOSAICO_GAME_RECOVERY_COMPONENT_DIR is not an ESP-IDF component: "
                "${MOSAICO_GAME_RECOVERY_COMPONENT_DIR}")
        endif()
        list(APPEND EXTRA_COMPONENT_DIRS "${MOSAICO_GAME_RECOVERY_COMPONENT_DIR}")
    endif()
    list(REMOVE_DUPLICATES EXTRA_COMPONENT_DIRS)
    set(EXTRA_COMPONENT_DIRS "${EXTRA_COMPONENT_DIRS}" PARENT_SCOPE)
endfunction()
