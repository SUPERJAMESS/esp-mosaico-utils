idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES esp_iris esp_mosaico_app_recovery esp_partition mbedtls nvs_flash)
set(stress_generated "${CMAKE_BINARY_DIR}/stress-inputs")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../generate_inputs.py")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../generate_inputs.py"
    "${STRESS_SAMPLE}" "${stress_generated}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot generate deterministic flash stress inputs")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${stress_generated}")
target_add_binary_data(${COMPONENT_LIB} "${stress_generated}/payload.bin" BINARY)
