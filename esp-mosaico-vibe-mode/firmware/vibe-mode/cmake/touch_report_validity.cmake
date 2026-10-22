# ACK-only and malformed frames are not contact-up reports. The pinned driver
# currently returns ESP_OK/zero points for them, splitting a held contact into
# repeated clicks. Compile a guarded generated copy; keep the download intact.
function(vibe_configure_touch_report_validity)
    idf_component_get_property(touch_dir espressif__esp_lcd_touch_cst9220 COMPONENT_DIR)
    idf_component_get_property(touch_lib espressif__esp_lcd_touch_cst9220 COMPONENT_LIB)
    set(original "${touch_dir}/esp_lcd_touch_cst9220.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" contents)
    set(before [=[    if (report[0] == CST9220_REPORT_ACK || report[CST9220_REPORT_HEADER_SIZE - 1] != CST9220_REPORT_ACK) {
        ESP_LOGW(TAG, "Malformed report header: first=0x%02x marker=0x%02x",
                 report[0], report[CST9220_REPORT_HEADER_SIZE - 1]);
        *point_num = 0;
        return ESP_OK;
    }]=])
    set(after [=[    if (report[0] == CST9220_REPORT_ACK || report[CST9220_REPORT_HEADER_SIZE - 1] != CST9220_REPORT_ACK) {
        /* No fresh valid report: preserve the caller's current contact. A
         * real release still arrives as a valid report and is decoded below. */
        *point_num = 0;
        return ESP_ERR_INVALID_RESPONSE;
    }]=])
    string(FIND "${contents}" "${before}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Review Vibe touch report override: pinned driver changed")
    endif()
    string(REPLACE "${before}" "${after}" patched "${contents}")
    string(FIND "${patched}" "${before}" remaining)
    if(NOT remaining LESS 0)
        message(FATAL_ERROR "Review Vibe touch report override: source replacement failed")
    endif()
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/vibe_touch_cst9220.c")
    set(previous "")
    if(EXISTS "${generated}")
        file(READ "${generated}" previous)
    endif()
    if(NOT previous STREQUAL patched)
        file(WRITE "${generated}" "${patched}")
    endif()
    get_target_property(sources ${touch_lib} SOURCES)
    set(replaced FALSE)
    set(patched_sources "")
    foreach(source IN LISTS sources)
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${touch_dir}")
        if(absolute STREQUAL original)
            list(APPEND patched_sources "${generated}")
            set(replaced TRUE)
        else()
            list(APPEND patched_sources "${source}")
        endif()
    endforeach()
    if(NOT replaced)
        message(FATAL_ERROR "Review Vibe touch report override: driver source target changed")
    endif()
    set_property(TARGET ${touch_lib} PROPERTY SOURCES "${patched_sources}")
endfunction()
vibe_configure_touch_report_validity()
