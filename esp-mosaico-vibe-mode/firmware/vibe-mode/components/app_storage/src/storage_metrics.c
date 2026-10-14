// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#if CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE
#include "storage_metrics.h"
#include "storage_spi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static uint64_t s_total[STORAGE_METRIC_COUNT], s_start[STORAGE_METRIC_COUNT];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static spi_device_handle_t s_spi;
static size_t s_page_size;
/* NAND commands are serialized by the driver's mutex. These timestamps track
 * page load -> first ready status; the interval includes status SPI time. */
static int64_t s_ready_started, s_raw_started, s_started;
static bool s_active;

void storage_metrics_add(storage_metric_t metric, uint64_t value) {
    portENTER_CRITICAL(&s_lock);
    s_total[metric] += value;
    portEXIT_CRITICAL(&s_lock);
}
void storage_metrics_bind(spi_device_handle_t spi, size_t page_size) {
    s_spi = spi;
    s_page_size = page_size;
    s_ready_started = 0;
    s_raw_started = 0;
}
void storage_metrics_begin(void) {
    portENTER_CRITICAL(&s_lock);
    memcpy(s_start, s_total, sizeof(s_start));
    s_active = true;
    s_started = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}
void storage_metrics_end(void) {
    uint64_t delta[STORAGE_METRIC_COUNT];
    portENTER_CRITICAL(&s_lock);
    if (!s_active) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    for (unsigned i = 0; i < STORAGE_METRIC_COUNT; ++i)
        delta[i] = s_total[i] - s_start[i];
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
#define M(key) (unsigned long long)delta[STORAGE_##key]
    ESP_LOGI("nand_perf", "page=%u total_us=%lld logical_pages=%llu hits=%llu misses=%llu logical_us=%llu raw_data=%llu raw_meta=%llu raw_bytes=%llu raw_us=%llu io_errors=%llu",
             (unsigned)s_page_size, (long long)(esp_timer_get_time() - s_started),
             M(LOGICAL_PAGES), M(CACHE_HITS), M(CACHE_MISSES), M(LOGICAL_US),
             M(RAW_DATA), M(RAW_META), M(RAW_BYTES), M(RAW_US), M(IO_ERRORS));
    ESP_LOGI("nand_perf", "spi_page=%llu spi_status=%llu spi_data=%llu spi_other=%llu page_us=%llu status_us=%llu data_us=%llu other_us=%llu spi_bytes=%llu ready_us=%llu polling=%llu spi_errors=%llu",
             M(SPI_PAGE), M(SPI_STATUS), M(SPI_DATA), M(SPI_OTHER), M(SPI_PAGE_US),
             M(SPI_STATUS_US), M(SPI_DATA_US), M(SPI_OTHER_US), M(SPI_BYTES), M(READY_US), M(POLLING), M(ERRORS));
#undef M
}

esp_err_t __real_spi_device_transmit(spi_device_handle_t, spi_transaction_t *);
esp_err_t __wrap_spi_device_transmit(spi_device_handle_t spi, spi_transaction_t *tx) {
    if (spi != s_spi || !s_spi || !tx)
        return __real_spi_device_transmit(spi, tx);
    storage_metric_t count = STORAGE_SPI_OTHER, duration = STORAGE_SPI_OTHER_US;
    if (tx->cmd == 0x13) { count = STORAGE_SPI_PAGE; duration = STORAGE_SPI_PAGE_US; }
    else if (tx->cmd == 0x0f && tx->addr == 0xc0) { count = STORAGE_SPI_STATUS; duration = STORAGE_SPI_STATUS_US; }
    else if (tx->cmd == 0x0b || tx->cmd == 0x3b || tx->cmd == 0xbb || tx->cmd == 0x6b || tx->cmd == 0xeb) {
        count = STORAGE_SPI_DATA; duration = STORAGE_SPI_DATA_US;
    }
    int64_t started = esp_timer_get_time();
    esp_err_t err;
#if CONFIG_IRIS_FACTORY_NAND_POLL_SHORT
    /* The NAND driver's mutex serializes callers, and each transmit completes
     * before the next starts: no queued transaction is left when entering
     * polling mode. Do not retry a failed transaction through the other API.
     * Read payloads still block on DMA, letting other tasks run every page.
     * The driver's BUSY timeout and ECC handling remain in charge. */
    if (storage_spi_short_read(tx)) {
        err = spi_device_polling_transmit(spi, tx);
        storage_metrics_add(STORAGE_POLLING, 1);
    } else
#endif
        err = __real_spi_device_transmit(spi, tx);
    int64_t ended = esp_timer_get_time();
    storage_metrics_add(count, 1);
    storage_metrics_add(duration, ended - started);
    storage_metrics_add(STORAGE_SPI_BYTES, (tx->rxlength + tx->length + 7) / 8);
    if (count == STORAGE_SPI_DATA && s_raw_started) {
        storage_metrics_add(tx->rxlength / 8 == s_page_size ? STORAGE_RAW_DATA : STORAGE_RAW_META, 1);
        storage_metrics_add(STORAGE_RAW_BYTES, tx->rxlength / 8);
        storage_metrics_add(STORAGE_RAW_US, ended - s_raw_started);
        s_raw_started = 0;
    }
    if (err != ESP_OK) {
        storage_metrics_add(STORAGE_ERRORS, 1);
        s_ready_started = 0;
        s_raw_started = 0;
    } else if (count == STORAGE_SPI_PAGE) {
        s_raw_started = started;
        s_ready_started = ended;
    } else if (count == STORAGE_SPI_STATUS && s_ready_started) {
        const uint8_t *status = tx->flags & SPI_TRANS_USE_RXDATA ? tx->rx_data : tx->rx_buffer;
        if (status && !(status[0] & 1)) {
            storage_metrics_add(STORAGE_READY_US, ended - s_ready_started);
            s_ready_started = 0;
        }
    } else if (count == STORAGE_SPI_OTHER) {
        s_ready_started = s_raw_started = 0;
    }
    return err;
}
#endif
