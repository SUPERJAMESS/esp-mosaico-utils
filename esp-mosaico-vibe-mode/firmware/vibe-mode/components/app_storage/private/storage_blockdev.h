// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_blockdev.h"

/* On success, the wrapper owns source, including release after mount failure.
 * Allocation failure for the optional PSRAM cache falls back to uncached IO. */
esp_err_t storage_blockdev_wrap(esp_blockdev_handle_t source, size_t cache_bytes,
                                esp_blockdev_handle_t *out);

/* Sticky physical read failure, reset only after successful recovery. */
esp_err_t storage_blockdev_read_error(void);
void storage_blockdev_clear_error(void);
