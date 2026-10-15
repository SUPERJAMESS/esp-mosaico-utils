// SPDX-License-Identifier: Apache-2.0
/* Hardware stubs for the device adapter's returned configuration contract.
 * This does not simulate GSP rendering or validate physical LCD transfers. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef int bsp_display_config_t;
enum {
    ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_STATE = 2,
    ESP_DISPLAY_PRESENT_PANEL_IO, ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
    ESP_DISPLAY_PRESENT_ROTATE_0, ESP_DISPLAY_PRESENT_MODE_AUTO,
    BSP_LCD_ROTATION_DEFAULT, BSP_LCD_TE = 8,
    BSP_LCD_PIXEL_CLOCK_HZ = 40000000, BSP_LCD_DATA_WIDTH = 4,
    BSP_LCD_H_RES = 480, BSP_LCD_V_RES = 480,
};
typedef struct {
    uint16_t x_pixels, y_pixels, width_pixels, height_pixels;
} esp_display_present_render_alignment_t;
typedef struct {
    struct {
        void *panel, *io;
        int panel_type, input_pixel_format, rotation;
        bool swap_bytes, te_enabled;
        struct { int gpio_num, bus_freq_hz, data_lines; } te_sync;
    } hw;
    struct { int mode; } fb;
} esp_display_present_target_config_t;
typedef struct {
    size_t struct_size;
    esp_display_present_target_config_t display;
    esp_lcd_touch_handle_t touch;
    esp_display_present_render_alignment_t render_alignment;
} esp_gsp_esp_lcd_config_t;
#define ESP_GSP_ESP_LCD_CONFIG_INIT() \
    ((esp_gsp_esp_lcd_config_t){.struct_size = sizeof(esp_gsp_esp_lcd_config_t)})
#define BSP_DISPLAY_DEFAULT_CONFIG() 0
#define ESP_RETURN_ON_FALSE(condition, error, ...) \
    do { if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expression, ...) \
    do { esp_err_t result = (expression); if (result != ESP_OK) return result; } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI

static int panel, io, touch;
static bool touch_available;
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
static esp_err_t bsp_display_new(const bsp_display_config_t *config,
                                 esp_lcd_panel_handle_t *out_panel)
{
    (void)config;
    *out_panel = &panel;
    return ESP_OK;
}
static void *bsp_display_get_panel_io(void) { return &io; }
static esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t *out_touch)
{
    (void)rotation;
    *out_touch = &touch;
    return touch_available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

#include "app_display.c"

int main(void)
{
    for (int available = 0; available <= 1; ++available) {
        touch_available = available != 0;
        esp_gsp_esp_lcd_config_t config = {0};
        assert(app_display_init(&config) == ESP_OK);
        assert(config.render_alignment.x_pixels == 4);
        assert(config.render_alignment.width_pixels == 4);
        assert(config.render_alignment.y_pixels == 0);
        assert(config.render_alignment.height_pixels == 0);
        assert(config.display.hw.panel == &panel);
        assert(config.touch == (touch_available ? &touch : NULL));
    }
    return 0;
}
