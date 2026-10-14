// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "driver/spi_master.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    STORAGE_LOGICAL_PAGES, STORAGE_CACHE_HITS, STORAGE_CACHE_MISSES,
    STORAGE_LOGICAL_US, STORAGE_RAW_DATA, STORAGE_RAW_META, STORAGE_RAW_BYTES,
    STORAGE_RAW_US, STORAGE_SPI_PAGE, STORAGE_SPI_STATUS, STORAGE_SPI_DATA,
    STORAGE_SPI_OTHER, STORAGE_SPI_PAGE_US, STORAGE_SPI_STATUS_US,
    STORAGE_SPI_DATA_US, STORAGE_SPI_OTHER_US, STORAGE_SPI_BYTES,
    STORAGE_READY_US, STORAGE_ERRORS, STORAGE_IO_ERRORS, STORAGE_POLLING, STORAGE_METRIC_COUNT
} storage_metric_t;
void storage_metrics_add(storage_metric_t metric, uint64_t value);
void storage_metrics_bind(spi_device_handle_t spi, size_t page_size);
void storage_metrics_begin(void);
void storage_metrics_end(void);
