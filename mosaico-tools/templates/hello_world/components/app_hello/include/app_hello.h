// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_vibe_settings.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque UI controller; ownership and threading contracts: ../README.md. */
typedef struct app_hello *app_hello_handle_t;

/** Value copy for diagnostics; no pointers into private controller state. */
typedef struct {
    uint32_t count;             /**< Button presses, 0..100; 100 during celebration. */
    bool celebrating;           /**< True while the 3-second animation is active. */
    bool settings_open;
    uint8_t brightness;         /**< Last settings snapshot, one of 20..100. */
    esp_gsp_err_t last_error;   /**< Most recent callback/setter error, or ESP_GSP_OK. */
} app_hello_snapshot_t;

/** Create a hello controller for ui; the caller owns *out_handle.
 * ui/out_handle must be non-NULL, and ui must outlive the controller.
 * Returns ESP_GSP_OK or a GSP error; clears *out_handle on failure. */
esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle);

/** Copy diagnostics to out_snapshot; both arguments must be non-NULL.
 * Call on the UI loop or after dispatch stops; this is not a cross-thread read.
 * Returns ESP_GSP_OK or ESP_GSP_ERR_INVALID_ARG; output unchanged on error. */
esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot);

/** Detach callbacks, release the controller and clear *handle.
 * Quiesce event/timer dispatch first and keep the borrowed ui alive.
 * Returns ESP_GSP_OK or a GSP error; retains the handle if cleanup fails. */
esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle);

/** UI-task callback. Must not block, touch NVS, or call back into GSP. */
typedef void (*app_hello_pref_cb_t)(app_vibe_cmd_t cmd, uint32_t value, void *ctx);
typedef void (*app_hello_activity_cb_t)(void *ctx);

/** Register the device worker. Either callback may be NULL. */
void app_hello_set_pref_handler(app_hello_handle_t handle, app_hello_pref_cb_t on_pref,
                                app_hello_activity_cb_t on_activity, void *ctx);

/** Ask the UI timer to open or close settings. Safe from the AI-key task. */
void app_hello_request_settings_toggle(app_hello_handle_t handle);

/** Queue a settings snapshot. The UI timer paints it. Does not draw itself. */
void app_hello_settings_post_view(app_hello_handle_t handle,
                                  const app_vibe_settings_view_t *view);

/** Copy and clear the password typed on the Wi-Fi keyboard. */
void app_hello_take_password(app_hello_handle_t handle, char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif
