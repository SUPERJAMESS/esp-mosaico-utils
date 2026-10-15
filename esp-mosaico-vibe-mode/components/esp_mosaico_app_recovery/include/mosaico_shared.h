// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read a value configured in Vibe Mode > Settings > Shared information.
 * Keys are 1..15 ASCII lowercase letters, digits or underscores. Values are
 * UTF-8 strings, at most 256 bytes plus NUL. A missing key returns
 * ESP_ERR_NVS_NOT_FOUND; insufficient capacity returns ESP_ERR_NVS_INVALID_LENGTH.
 * Task context only. This never writes, erases, logs or caches the value.
 * Shared information is common to all Ideas and is cleared by factory reset.
 */
esp_err_t mosaico_shared_get(const char *key, char *value, size_t capacity);

#ifdef __cplusplus
}
#endif
