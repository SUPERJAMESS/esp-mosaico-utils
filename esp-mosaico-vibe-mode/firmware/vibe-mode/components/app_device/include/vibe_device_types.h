// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool battery_valid, charging, usb_connected, imu_available, haptics_available;
    bool peripherals_ready;
    unsigned battery_percent, orientation;
    uint32_t motion_revision;
} vibe_device_snapshot_t;
