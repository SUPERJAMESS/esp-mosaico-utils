// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define APP_STORAGE_MOUNT_PATH "/nand"
#define APP_STORAGE_VOLUME_ID "nand"

/* Mount the retained filesystem without formatting. Call during startup, or
 * while holding the reservation. Returns a media error without erasing data. */
esp_err_t app_storage_mount(void);
/* Nonblocking admission for a local NAND job. ESP_OK transfers the reservation
 * to the caller, which must release it on all completion/failure paths. */
esp_err_t app_storage_reserve(void);
/* Reserve and mount for synchronous filesystem access. On failure no lease is
 * retained. May block for IO; never call from the UI render task. */
esp_err_t app_storage_acquire(void);
/* Release a successful reserve/acquire once all caller IO has completed. */
void app_storage_release(void);
