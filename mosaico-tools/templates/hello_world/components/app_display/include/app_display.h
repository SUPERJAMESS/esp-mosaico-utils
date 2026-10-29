// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"
#include "esp_gsp_esp_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the BSP display and optional touch; fill non-NULL out_config.
 * Call once at boot before GSP; hardware ownership and timing: ../README.md.
 * Returns ESP_OK or an initialization error; output unchanged on failure. */
esp_err_t app_display_init(esp_gsp_esp_lcd_config_t *out_config);

/** Remember the running UI so brightness and rotation can pause the presenter. */
void app_display_attach(esp_gsp_handle_t ui);

/** Pause GSP, then set the backlight to 0..100 percent. */
esp_err_t app_display_set_brightness(unsigned percent);

/** Pause GSP and rotate the presenter plus touch. degrees is 0, 90, 180 or 270. */
esp_err_t app_display_set_rotation(unsigned degrees);

#ifdef __cplusplus
}
#endif
