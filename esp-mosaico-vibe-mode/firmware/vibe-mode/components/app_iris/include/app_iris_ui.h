// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    void *ctx;
    /* Called from Iris worker; must marshal work to the render task. */
    esp_err_t (*open_download)(void *ctx);
    /* Called from Iris worker; validates bounds and queues pointer input. */
    esp_err_t (*touch)(void *ctx, int16_t x, int16_t y, bool pressed);
} app_iris_ui_t;
/* Register USB pointer input after UI startup. Copies callbacks; ctx and its
 * target must remain alive for the lifetime of the Iris service. */
esp_err_t app_iris_input_register(const app_iris_ui_t *ui);
