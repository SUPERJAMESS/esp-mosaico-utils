// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(__has_include)
#  if __has_include("esp_err.h")
#    include "esp_err.h"
#  endif
#endif
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Commands the settings surface posts. The worker applies them off the UI task. */
typedef enum {
    APP_VIBE_CMD_BRIGHTNESS = 1,
    APP_VIBE_CMD_TIMEOUT,
    APP_VIBE_CMD_ROTATE,
    APP_VIBE_CMD_HAPTICS,
    APP_VIBE_CMD_LANG,
    APP_VIBE_CMD_WIFI_SCAN,
    APP_VIBE_CMD_WIFI_PICK,
    APP_VIBE_CMD_WIFI_JOIN,
    APP_VIBE_CMD_WIFI_FORGET,
    APP_VIBE_CMD_RELOAD,
    APP_VIBE_CMD_VOLUME,
    APP_VIBE_CMD_HAPTIC_FEEDBACK,
} app_vibe_cmd_t;

/** Hardware apply requested by the worker. main routes this to the display. */
typedef enum {
    APP_VIBE_APPLY_BRIGHTNESS = 1,
    APP_VIBE_APPLY_ROTATION,
    APP_VIBE_APPLY_HAPTICS,
    APP_VIBE_APPLY_HAPTIC_FEEDBACK,
} app_vibe_apply_t;

/** Value copy of the shared sysmeta surface. No pointers into worker state. */
typedef struct {
    uint8_t brightness; /**< 20, 40, 60, 80 or 100. Missing key shows 80. */
    uint16_t timeout_s; /**< 0, 60, 180, 300 or 600. Other stored values show 0. */
    bool rotate;
    bool haptics;
    uint8_t lang; /**< 1 English, 2 Simplified Chinese. */
    uint8_t volume; /**< 0, 20, 40, 60, 80 or 100. App NVS "hello", not vibe_prefs. */
    char wifi_line[48];
    char wifi_detail[48];
    char wifi_ssid[33];
    char wifi_ap[12][33];
    uint8_t wifi_open[12];
    uint8_t wifi_ap_count;
    char shared_line[8][48];
    uint8_t shared_count;
} app_vibe_settings_view_t;

typedef void (*app_vibe_settings_view_cb_t)(const app_vibe_settings_view_t *view, void *ctx);
typedef void (*app_vibe_settings_apply_cb_t)(app_vibe_apply_t what, uint32_t value, void *ctx);
typedef void (*app_vibe_settings_toggle_cb_t)(void *ctx);
typedef void (*app_vibe_settings_activity_cb_t)(void *ctx);

/** Which shared groups this process serves. All-false enables every group. */
typedef struct {
    bool wifi;
    bool display;
    bool language;
    bool haptics;
    bool shared;
    app_vibe_settings_view_cb_t on_view;     /**< May be NULL. Not the UI task. */
    app_vibe_settings_apply_cb_t on_apply;   /**< May be NULL. Settings task. */
    app_vibe_settings_toggle_cb_t on_toggle; /**< AI key. May be NULL. */
    app_vibe_settings_activity_cb_t on_activity;
    void *ctx;
} app_vibe_settings_config_t;

/** Start the sysmeta worker. Call once after NVS init. Stack is 6144 bytes. */
esp_err_t app_vibe_settings_start(const app_vibe_settings_config_t *config);

/** Queue one command. Non-blocking; safe from the UI task. */
void app_vibe_settings_request(app_vibe_cmd_t cmd, uint32_t value);

/** Store a pending Wi-Fi password for the next JOIN. Does not log it. */
void app_vibe_settings_set_password(const char *password);

/** Write vibe_prefs/haptics. Motor strength 0 in an app calls this with false. */
void app_vibe_settings_set_haptics(bool enabled);

/** Reset the idle dim timer. Safe from the UI task. */
void app_vibe_settings_note_activity(void);

#ifdef __cplusplus
}
#endif
