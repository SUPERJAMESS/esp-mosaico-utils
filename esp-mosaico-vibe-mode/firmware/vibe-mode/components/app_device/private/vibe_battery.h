// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"
/* Private checked gauge reader. The app_device worker owns its lifetime. */
typedef struct {
    uint16_t state_of_charge, status_flags;
    int16_t voltage_mv, current_ma;
} vibe_battery_sample_t;
esp_err_t vibe_battery_init(void);
esp_err_t vibe_battery_read(vibe_battery_sample_t *out);
esp_err_t vibe_battery_deinit(void);
