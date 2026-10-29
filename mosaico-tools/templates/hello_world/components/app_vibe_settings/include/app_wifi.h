// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WIFI_AP_MAX 12

/** Start STA once and connect saved sysmeta credentials when they exist.
 * The settings task is the only caller. */
esp_err_t app_wifi_start(void);

/** Blocking scan. Copies up to APP_WIFI_AP_MAX SSIDs. Does not log values. */
esp_err_t app_wifi_scan(char out[APP_WIFI_AP_MAX][33], uint8_t *count);

/** Remember a scanned SSID by index. */
esp_err_t app_wifi_select(uint8_t index);

/** Replace the pending password. Empty is allowed for an open network. */
void app_wifi_set_password(const char *text);

/** Validate, store wifi_ssid/wifi_password, and connect. */
esp_err_t app_wifi_join(void);

/** Erase the saved network and disconnect. */
esp_err_t app_wifi_forget(void);

void app_wifi_copy(char out[APP_WIFI_AP_MAX][33], uint8_t *open, uint8_t *count);
const char *app_wifi_selected(void);
void app_wifi_status(char *dst, size_t dst_len, uint8_t lang);
void app_wifi_detail(char *dst, size_t dst_len, uint8_t lang);

#ifdef __cplusplus
}
#endif
