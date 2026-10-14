// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "vibe_device_types.h"
/* Start board sampling after display/UI startup. Failures of optional sensors
 * remain explicit in snapshots and never disable the maintenance transport. */
/* Rotate callback runs on the device service task, outside the render task. */
esp_err_t vibe_device_start(esp_err_t (*rotate)(unsigned degrees));
esp_err_t vibe_device_snapshot(vibe_device_snapshot_t *out);
/* Nonblocking desired state, applied by the board worker. */
void vibe_device_policy(bool asleep, bool rotation_allowed);
void vibe_device_haptic(unsigned pattern);
/* 1/2 AI down/up, 3/4 Boot down/up. No simulated power-button events on device. */
unsigned vibe_device_poll_key(void);
