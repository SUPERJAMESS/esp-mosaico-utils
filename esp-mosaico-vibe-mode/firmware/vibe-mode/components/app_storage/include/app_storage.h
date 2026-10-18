// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define APP_STORAGE_MOUNT_PATH "/nand"
#define APP_STORAGE_VOLUME_ID "nand"

/* Mount the retained filesystem without formatting while holding the reservation.
 * Returns a media error without erasing data. */
esp_err_t app_storage_mount(void);
/* Nonblocking mount status: INVALID_STATE while pending, OK once mounted,
 * otherwise a mount failure. Mounted media remains available for life. */
esp_err_t app_storage_status(void);
/* Nonblocking admission for a local NAND job. ESP_OK transfers the reservation
 * to the caller, which must release it on all completion/failure paths. */
esp_err_t app_storage_reserve(void);
/* Reserve and mount for synchronous filesystem access. On failure no lease is
 * retained. May block for IO; never call from the UI render task. */
esp_err_t app_storage_acquire(void);
/* Release a successful reserve/acquire once all caller IO has completed. */
void app_storage_release(void);
/* Read filesystem usage while holding a storage reservation. */
esp_err_t app_storage_info(uint64_t *total, uint64_t *used);
