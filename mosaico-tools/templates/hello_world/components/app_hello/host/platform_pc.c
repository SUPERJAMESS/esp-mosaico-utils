// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <string.h>
#include "app_hello.h"
#include "esp_gsp.h"
#include "gsp_sim_bridge.h"

esp_gsp_err_t esp_gsp_keyboard_attach_ex(esp_gsp_handle_t gsp, uint16_t action_id,
                                         uint16_t text_bind, size_t max_bytes)
{
    (void)gsp;
    (void)action_id;
    (void)text_bind;
    (void)max_bytes;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_keyboard_text(esp_gsp_handle_t gsp, char *out_text, size_t capacity)
{
    (void)gsp;
    if (!out_text || capacity == 0) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';
    return ESP_GSP_OK;
}

static struct {
    app_hello_handle_t controller;
    app_vibe_settings_view_t view;
} backend;

static void publish(void)
{
    app_hello_settings_post_view(backend.controller, &backend.view);
}

static void host_pref(app_vibe_cmd_t cmd, uint32_t value, void *ctx)
{
    (void)ctx;
    if (cmd == APP_VIBE_CMD_BRIGHTNESS && value >= 20 && value <= 100 && value % 20 == 0) {
        backend.view.brightness = (uint8_t)value;
    } else if (cmd == APP_VIBE_CMD_TIMEOUT &&
               (value == 0 || value == 60 || value == 180 || value == 300 || value == 600)) {
        backend.view.timeout_s = (uint16_t)value;
    } else if (cmd == APP_VIBE_CMD_ROTATE) {
        backend.view.rotate = value == 1;
    } else if (cmd == APP_VIBE_CMD_HAPTICS) {
        backend.view.haptics = value == 1;
    } else if (cmd == APP_VIBE_CMD_LANG && (value == 1 || value == 2)) {
        backend.view.lang = (uint8_t)value;
    } else if (cmd == APP_VIBE_CMD_VOLUME && value <= 100 && value % 20 == 0) {
        backend.view.volume = (uint8_t)value;
    } else if (cmd == APP_VIBE_CMD_WIFI_SCAN || cmd == APP_VIBE_CMD_RELOAD) {
        snprintf(backend.view.wifi_detail, sizeof(backend.view.wifi_detail),
                 "Choose a network below");
    } else if (cmd == APP_VIBE_CMD_WIFI_FORGET) {
        backend.view.wifi_line[0] = '\0';
        snprintf(backend.view.wifi_line, sizeof(backend.view.wifi_line), "Not connected");
        backend.view.wifi_ssid[0] = '\0';
    } else if (cmd == APP_VIBE_CMD_WIFI_JOIN) {
        char secret[65];
        app_hello_take_password(backend.controller, secret, sizeof(secret));
        memset(secret, 0, sizeof(secret));
        if (backend.view.wifi_ssid[0]) {
            snprintf(backend.view.wifi_line, sizeof(backend.view.wifi_line), "%s",
                     backend.view.wifi_ssid);
            snprintf(backend.view.wifi_detail, sizeof(backend.view.wifi_detail), "Saved");
        }
    }
    publish();
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    esp_gsp_err_t err = app_hello_create(ui, &backend.controller);
    if (err != ESP_GSP_OK) {
        return err;
    }
    backend.view.brightness = 60;
    backend.view.volume = 80;
    backend.view.haptics = true;
    backend.view.lang = 1;
    snprintf(backend.view.wifi_line, sizeof(backend.view.wifi_line), "Not connected");
    snprintf(backend.view.wifi_detail, sizeof(backend.view.wifi_detail),
             "Choose a network below");
    app_hello_set_pref_handler(backend.controller, host_pref, NULL, NULL);
    publish();
    return ESP_GSP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    app_hello_snapshot_t snapshot;
    if (app_hello_read(backend.controller, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr, "hello_backend: count=%u settings=%d brightness=%u last_error=%d\n",
                (unsigned)snapshot.count, snapshot.settings_open ? 1 : 0,
                (unsigned)snapshot.brightness, (int)snapshot.last_error);
    }
    const esp_gsp_err_t err = app_hello_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "hello_backend: cleanup failed (%d)\n", err);
    }
}
