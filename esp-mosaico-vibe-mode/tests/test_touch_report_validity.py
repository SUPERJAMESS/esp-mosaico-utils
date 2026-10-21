"""Compile the product-patched pinned driver's report reader with an I2C stub."""
from pathlib import Path
import subprocess

import pytest


FIRMWARE = Path(__file__).resolve().parents[1] / 'firmware/vibe-mode'


def test_invalid_report_is_not_a_release(tmp_path):
    source = FIRMWARE / 'managed_components/espressif__esp_lcd_touch_cst9220/esp_lcd_touch_cst9220.c'
    if not source.exists():
        pytest.skip('Requires the pinned touch component fetched by an IDF configure')
    original = source.read_text()
    (tmp_path / source.name).write_text(original)
    (tmp_path / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(touch_report_test C)
add_library(driver OBJECT esp_lcd_touch_cst9220.c)
function(idf_component_get_property output component property)
    if(property STREQUAL "COMPONENT_DIR")
        set(${output} "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)
    else()
        set(${output} driver PARENT_SCOPE)
    endif()
endfunction()
include("''' + (FIRMWARE / 'cmake/touch_report_validity.cmake').as_posix() + '''")
''')
    build = tmp_path / 'build'
    subprocess.run(['cmake', '-S', str(tmp_path), '-B', str(build)], check=True, capture_output=True)
    generated = (build / 'vibe_touch_cst9220.c').read_text()
    assert source.read_text() == original, 'Dependency download was modified'
    start = generated.index('static esp_err_t cst9220_read_report(', generated.index('static esp_err_t cst9220_read_report(') + 1)
    end = generated.index('{', start) + 1
    depth = 1
    while depth:
        depth += (generated[end] == '{') - (generated[end] == '}')
        end += 1
    function = generated[start:end]
    test = tmp_path / 'report.c'
    test.write_text('''#include <assert.h>
#include <stdint.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE 1
#define CST9220_REG_REPORT 0
#define CST9220_REPORT_BUFFER_SIZE 64
#define CST9220_REPORT_HEADER_SIZE 7
#define CST9220_REPORT_ACK 0xab
#define CST9220_PROTOCOL_LEGACY 0
#define ESP_LCD_TOUCH_CST9220_MAX_POINTS 5
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { int unused; } base_t;
typedef base_t *esp_lcd_touch_handle_t;
typedef struct { base_t base; int protocol; } cst9220_touch_t;
static uint8_t frame[64];
static int ack_error;
static int cst9220_read_register(esp_lcd_touch_handle_t t, int reg, uint8_t *out, int size)
{ (void)t; (void)reg; memcpy(out, frame, size); return ESP_OK; }
static int cst9220_write_report_ack(esp_lcd_touch_handle_t t) { (void)t; return ack_error; }
''' + function + '''
int main(void) {
    cst9220_touch_t touch = {.protocol=1}; uint8_t report[64], count=0;
    frame[0]=6; frame[5]=1; frame[6]=0xab;
    assert(cst9220_read_report(&touch,report,&count)==ESP_OK && count==1);
    for(int i=0;i<20;++i) {
        frame[0]=0xab; frame[6]=0xab;
        assert(cst9220_read_report(&touch,report,&count)!=ESP_OK);
    }
    frame[0]=6; frame[6]=0;
    assert(cst9220_read_report(&touch,report,&count)!=ESP_OK);
    frame[0]=6; frame[6]=0xab;
    assert(cst9220_read_report(&touch,report,&count)==ESP_OK && count==1);
    frame[0]=0; frame[5]=0;
    assert(cst9220_read_report(&touch,report,&count)==ESP_OK && count==0);
    ack_error=2;
    assert(cst9220_read_report(&touch,report,&count)==2);
}
''')
    executable = tmp_path / 'report'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(executable)], check=True, capture_output=True)
    subprocess.run([str(executable)], check=True)
