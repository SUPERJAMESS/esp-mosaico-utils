// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    /* usb_connected means configured and not bus-suspended; it is independent
     * of charging and whether an Agent owns the link. */
    bool battery_valid, charging, usb_connected, imu_available, haptics_available;
    bool peripherals_ready;
    unsigned battery_percent, orientation;
    uint32_t motion_revision;
} vibe_device_snapshot_t;
