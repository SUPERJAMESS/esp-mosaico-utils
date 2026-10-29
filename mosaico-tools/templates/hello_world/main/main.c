// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "app_display.h"
#include "app_hello.h"
#include "app_vibe_settings.h"
#include "bsp/motor.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "hello_settings";

static struct {
    esp_gsp_handle_t ui;
    app_hello_handle_t controller;
} application;

static void on_pref(app_vibe_cmd_t cmd, uint32_t value, void *ctx)
{
    (void)ctx;
    app_vibe_settings_note_activity();
    if (cmd == APP_VIBE_CMD_WIFI_JOIN) {
        char secret[65];
        app_hello_take_password(application.controller, secret, sizeof(secret));
        app_vibe_settings_set_password(secret);
        memset(secret, 0, sizeof(secret));
    }
    app_vibe_settings_request(cmd, value);
}

static void on_view(const app_vibe_settings_view_t *view, void *ctx)
{
    (void)ctx;
    app_hello_settings_post_view(application.controller, view);
}

static esp_timer_handle_t s_motor_off;
static bool s_motor_ready;

static void motor_off(void *arg)
{
    (void)arg;
    if (s_motor_ready) {
        (void)bsp_motor_set_strength(0);
    }
}

static void pulse_motor(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = motor_off,
        .name = "hello_motor_off",
    };
    if (!s_motor_ready) {
        if (bsp_motor_init() != ESP_OK) {
            ESP_LOGW(TAG, "motor unavailable");
            return;
        }
        if (esp_timer_create(&timer_args, &s_motor_off) != ESP_OK) {
            ESP_LOGW(TAG, "motor timer unavailable");
            return;
        }
        s_motor_ready = true;
    }
    (void)esp_timer_stop(s_motor_off);
    (void)bsp_motor_set_strength(64);
    (void)esp_timer_start_once(s_motor_off, 60 * 1000);
}

static void on_apply(app_vibe_apply_t what, uint32_t value, void *ctx)
{
    (void)ctx;
    if (what == APP_VIBE_APPLY_BRIGHTNESS) {
        (void)app_display_set_brightness(value);
    } else if (what == APP_VIBE_APPLY_ROTATION) {
        (void)app_display_set_rotation(value);
    } else if (what == APP_VIBE_APPLY_HAPTICS ||
               what == APP_VIBE_APPLY_HAPTIC_FEEDBACK) {
        (void)value;
        pulse_motor();
    }
}

static void on_toggle(void *ctx)
{
    (void)ctx;
    app_hello_request_settings_toggle(application.controller);
}

void app_main(void)
{
    const app_vibe_settings_config_t settings = {
        .wifi = true,
        .display = true,
        .language = true,
        .haptics = true,
        .shared = false,
        .on_view = on_view,
        .on_apply = on_apply,
        .on_toggle = on_toggle,
        .on_activity = NULL,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    app_display_attach(application.ui);
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    ESP_ERROR_CHECK(app_hello_create(application.ui, &application.controller));
    app_hello_set_pref_handler(application.controller, on_pref, NULL, NULL);
    if (app_vibe_settings_start(&settings) != ESP_OK) {
        ESP_LOGW(TAG, "settings unavailable; the hello counter still runs");
    }
    ESP_LOGI(TAG, "GSP Hello World ready");
}
