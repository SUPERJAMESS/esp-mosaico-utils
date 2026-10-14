// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Read the product's one U32 boot intent; never modify NVS from the loader. */
esp_err_t mosaico_boot_read_intent(uint32_t *intent);
