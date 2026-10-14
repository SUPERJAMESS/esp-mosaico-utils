// SPDX-License-Identifier: Apache-2.0
#include "factory_ui.h"
#include "factory_ui_dispatch.h"
#include "vibe_ui.h"
#include "board_display.h"
#include "vibe_bundle.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_check.h"
#include "iris_screen_mirror.h"

#include "ui_services.h"
#include "esp_gsp_debug.h"
#include "bsp/esp_mosaico.h"
static const char *TAG = "vibe_ui";
typedef struct {
    esp_gsp_handle_t ui;
} ui_device_context_t;
static ui_device_context_t s_device;
/* Resume failures leave the token owned by us; retry without losing the UI. */
static esp_gsp_esp_lcd_pause_t *s_rotation_pause;
static unsigned s_rotation_degrees;

esp_err_t factory_ui_rotate(unsigned degrees)
{
    if (!s_device.ui || (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270))
        return ESP_ERR_INVALID_ARG;
    if (s_rotation_pause) {
        esp_err_t resumed = esp_gsp_esp_lcd_resume_paused(s_rotation_pause, &s_device.ui);
        if (resumed != ESP_OK) return resumed;
        s_rotation_pause = NULL;
        if (s_rotation_degrees == degrees) return ESP_OK;
    }
    esp_err_t err = esp_gsp_esp_lcd_pause(s_device.ui, 500, &s_rotation_pause);
    if (err != ESP_OK) return err;
    err = bsp_display_set_rotation((bsp_display_rotation_t)degrees);
    if (err == ESP_OK) s_rotation_degrees = degrees;
    esp_err_t resumed = esp_gsp_esp_lcd_resume_paused(s_rotation_pause, &s_device.ui);
    if (resumed == ESP_OK) s_rotation_pause = NULL;
    return resumed != ESP_OK ? resumed : err;
}

esp_err_t factory_ui_start(void)
{
    esp_display_present_target_config_t display;
    ESP_RETURN_ON_ERROR(board_display_init(&display), TAG, "display");
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(board_touch_init(&touch), TAG, "touch");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen backend");
    esp_gsp_config_t config;
    ESP_RETURN_ON_ERROR(vibe_bundle_open(&config), TAG, "embedded GSP bundle");
    /* GSPC counts the template's image once. The library needs independent
     * targets for every retained row, including rows recycled while scrolling. */
    if (esp_gsp_config_set(&config, ESP_GSP_FIELD_DEFAULT_DYNAMIC_IMAGE_SLOTS, VIBE_BUNDLE_MAX) !=
        ESP_GSP_CONFIG_SET_OK)
        return ESP_ERR_INVALID_STATE;
    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;
    esp_gsp_handle_t ui;
    ESP_RETURN_ON_ERROR(esp_gsp_esp_lcd_start(&config, &lcd, &ui), TAG, "GSP start");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(ui), TAG, "screen attach");
    s_device.ui = ui;
    ESP_RETURN_ON_ERROR(factory_ui_dispatch_start(ui, factory_ui_services()), TAG, "Vibe UI");
    ESP_LOGI(TAG, "Vibe Mode GSP UI ready at 480x480");
    return ESP_OK;
}
esp_err_t factory_ui_open_bridge(void *ctx)
{
    (void)ctx;
    return factory_ui_dispatch_open_bridge();
}

esp_err_t factory_ui_inject_touch(void *ctx, int16_t x, int16_t y, bool pressed)
{
    (void)ctx;
    if (!s_device.ui)
        return ESP_ERR_INVALID_STATE;
    if (x < 0 || x >= BSP_LCD_H_RES || y < 0 || y >= BSP_LCD_V_RES)
        return ESP_ERR_INVALID_ARG;
    return esp_gsp_inject_touch(s_device.ui, x, y, pressed);
}
