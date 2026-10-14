// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "driver/spi_master.h"
#include <stdbool.h>

/* Only NAND page-load commands and one-byte READY status queries. Page data,
 * programming and erase transactions keep the IDF interrupt/DMA path. */
static inline bool storage_spi_short_read(const spi_transaction_t *tx) {
    return tx && tx->length == 0 &&
           ((tx->cmd == 0x13 && tx->rxlength == 0) ||
            (tx->cmd == 0x0f && tx->addr == 0xc0 && tx->rxlength == 8));
}
