// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared ABI 2 boot policy. These functions never restart the processor.
 * NVS commits are atomic. The caller must serialize transitions across tasks
 * and owns restart order; these are not ISR or panic-handler entry points. */
esp_err_t mosaico_boot_request_recovery(void);
esp_err_t mosaico_boot_select(uint32_t address);
/* Call early in Vibe Mode, before peripherals and update services start. */
esp_err_t mosaico_boot_consume_request(void);
esp_err_t mosaico_boot_recovery_requested(bool *requested);

#ifdef __cplusplus
}
#endif
