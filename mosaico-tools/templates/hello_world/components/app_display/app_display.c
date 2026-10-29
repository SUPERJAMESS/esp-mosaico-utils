// SPDX-License-Identifier: Apache-2.0

#include "app_display.h"

#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_display_present.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "gsp_board";
static esp_display_presenter_t *s_presenter;
static esp_lcd_touch_handle_t s_touch;
static esp_gsp_handle_t s_ui;
static unsigned s_degrees;

static esp_err_t board_display_init(esp_display_present_target_config_t *out_target)
{
    ESP_RETURN_ON_FALSE(out_target != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "present target is null");

    const bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(&config, &panel), TAG,
                        "initialize CO5300");
    ESP_RETURN_ON_FALSE(panel != NULL && bsp_display_get_panel_io() != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "panel/io missing");

    *out_target = (esp_display_present_target_config_t) {
        .hw = {
            .panel = panel,
            .io = bsp_display_get_panel_io(),
            .panel_type = ESP_DISPLAY_PRESENT_PANEL_IO,
            .input_pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
            .rotation = ESP_DISPLAY_PRESENT_ROTATE_0,
            .swap_bytes = true,
#if CONFIG_BSP_CO5300_ENABLE_TE
            .te_enabled = true,
            .te_sync = {
                .gpio_num = BSP_LCD_TE,
                .bus_freq_hz = BSP_LCD_PIXEL_CLOCK_HZ,
                .data_lines = BSP_LCD_DATA_WIDTH,
            },
#endif
        },
        .fb = {
            .mode = ESP_DISPLAY_PRESENT_MODE_AUTO,
        },
    };
    const char *te_mode = "off";
    int te_gpio = -1;
#if CONFIG_BSP_CO5300_ENABLE_TE
    te_mode = "on";
    te_gpio = BSP_LCD_TE;
#endif
    ESP_LOGI(TAG, "GSP present target %dx%d RGB565 TE=%s GPIO=%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES, te_mode, te_gpio);
    return ESP_OK;
}

static esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(out_touch != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch output is null");
    const esp_err_t err = bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, out_touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable (%s); continuing without it",
                 esp_err_to_name(err));
        *out_touch = NULL;
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t app_display_init(esp_gsp_esp_lcd_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config != NULL, ESP_ERR_INVALID_ARG, TAG, "output is null");
    esp_gsp_esp_lcd_config_t config = ESP_GSP_ESP_LCD_CONFIG_INIT();
    /* CO5300 QSPI requires both the X origin and width in multiples of 4 pixels.
     * GSP defaults to no expansion; the BSP rounder only covers LVGL. */
    config.render_alignment = (esp_display_present_render_alignment_t) {
        .x_pixels = 4,
        .width_pixels = 4,
    };
    ESP_RETURN_ON_ERROR(board_display_init(&config.display), TAG, "display initialization");
    ESP_RETURN_ON_ERROR(board_touch_init(&config.touch), TAG, "touch initialization");
    const esp_display_presenter_config_t presenter = {
        .width = BSP_LCD_H_RES,
        .height = BSP_LCD_V_RES,
        .pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
        .max_damage_areas = 32,
        .target = config.display,
    };
    ESP_RETURN_ON_ERROR(esp_display_presenter_create(&presenter, &s_presenter), TAG,
                        "presenter");
    config.presenter = s_presenter;
    s_touch = config.touch;
    *out_config = config;
    return ESP_OK;
}

void app_display_attach(esp_gsp_handle_t ui)
{
    s_ui = ui;
}

static esp_err_t with_presenter_paused(esp_err_t (*work)(void *ctx), void *ctx)
{
    if (!s_ui) {
        return work(ctx);
    }
    esp_gsp_esp_lcd_pause_t *pause = NULL;
    esp_err_t err = esp_gsp_esp_lcd_pause(s_ui, 500, &pause);
    if (err != ESP_OK) {
        return err;
    }
    err = work(ctx);
    esp_gsp_handle_t ui = NULL;
    const esp_err_t resumed = esp_gsp_esp_lcd_resume_paused(pause, &ui);
    return err != ESP_OK ? err : resumed;
}

static esp_err_t write_brightness(void *ctx)
{
    return bsp_display_brightness_set((int)(uintptr_t)ctx);
}

esp_err_t app_display_set_brightness(unsigned percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return with_presenter_paused(write_brightness, (void *)(uintptr_t)percent);
}

static void touch_for_rotation(unsigned degrees)
{
    (void)degrees;
    if (!s_touch) {
        return;
    }
    /* GSP maps panel samples through esp_display_presenter_map_point. Driver
     * swap/mirror would rotate that sample a second time. */
    (void)esp_lcd_touch_set_swap_xy(s_touch, false);
    (void)esp_lcd_touch_set_mirror_x(s_touch, false);
    (void)esp_lcd_touch_set_mirror_y(s_touch, false);
}

static esp_err_t write_rotation(void *ctx)
{
    const unsigned degrees = (unsigned)(uintptr_t)ctx;
    if (!s_presenter) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_display_presenter_set_rotation(
        s_presenter, (esp_display_present_rotation_t)degrees);
    if (err == ESP_OK) {
        s_degrees = degrees;
        touch_for_rotation(degrees);
    }
    return err;
}

esp_err_t app_display_set_rotation(unsigned degrees)
{
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        return ESP_ERR_INVALID_ARG;
    }
    if (degrees == s_degrees) {
        return ESP_OK;
    }
    return with_presenter_paused(write_rotation, (void *)(uintptr_t)degrees);
}
