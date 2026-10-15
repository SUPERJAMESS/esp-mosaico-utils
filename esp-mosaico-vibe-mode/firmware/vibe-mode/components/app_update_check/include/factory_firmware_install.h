// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "factory_update_check.h"
#include "esp_err.h"

typedef enum {
    FIRMWARE_IDLE, FIRMWARE_DOWNLOADING, FIRMWARE_VERIFYING,
    FIRMWARE_WRITING, FIRMWARE_RESTARTING, FIRMWARE_FAILED
} factory_firmware_phase_t;

typedef struct {
    factory_firmware_phase_t phase;
    uint32_t received, total, revision;
    esp_err_t error;
} factory_firmware_status_t;

/* Copies the selected release and reserves the writer before returning. */
esp_err_t factory_firmware_install_start(const factory_update_check_snapshot_t *release);
void factory_firmware_install_status(factory_firmware_status_t *out);
