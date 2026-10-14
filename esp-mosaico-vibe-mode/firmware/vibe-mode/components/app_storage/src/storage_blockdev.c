// SPDX-License-Identifier: Apache-2.0
#include "storage_blockdev.h"
#include "storage_metrics.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t address, age; } cache_entry_t;
typedef struct {
    esp_blockdev_t device;
    esp_blockdev_handle_t source;
    SemaphoreHandle_t lock;
    cache_entry_t *entries;
    uint8_t *data;
    size_t page_size, count;
    uint64_t clock;
} storage_blockdev_t;

static void invalidate(storage_blockdev_t *s) {
    if (s->entries) memset(s->entries, 0, s->count * sizeof(*s->entries));
    s->clock = 0;
}
static esp_err_t read_pages(esp_blockdev_handle_t dev, uint8_t *dst, size_t capacity,
                            uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    if (!dst || capacity < size || address > dev->geometry.disk_size ||
        size > dev->geometry.disk_size - address ||
        address % s->page_size || size % s->page_size) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    const int64_t started = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    storage_metrics_add(STORAGE_LOGICAL_PAGES, size / s->page_size);
    for (size_t offset = 0; offset < size; offset += s->page_size) {
        size_t victim = 0, hit = s->count;
        /* A wrap is practically unreachable, but must never create age=0 for
         * a valid entry: zero is the invalid marker. */
        if (s->clock == UINT64_MAX) invalidate(s);
        for (size_t i = 0; i < s->count; ++i) {
            if (s->entries[i].age && s->entries[i].address == address + offset) {
                hit = i;
                break;
            }
            if (s->entries[i].age < s->entries[victim].age) victim = i;
        }
        if (hit < s->count) {
            memcpy(dst + offset, s->data + hit * s->page_size, s->page_size);
            s->entries[hit].age = ++s->clock;
            storage_metrics_add(STORAGE_CACHE_HITS, 1);
            continue;
        }
        storage_metrics_add(STORAGE_CACHE_MISSES, 1);
        uint8_t *buffer = s->count ? s->data + victim * s->page_size : dst + offset;
        if (s->count) s->entries[victim].age = 0;
        err = s->source->ops->read(s->source, buffer, s->page_size, address + offset, s->page_size);
        if (err != ESP_OK) {
            storage_metrics_add(STORAGE_IO_ERRORS, 1);
            invalidate(s);
            break;
        }
        if (s->count) {
            s->entries[victim] = (cache_entry_t){address + offset, ++s->clock};
            memcpy(dst + offset, buffer, s->page_size);
        }
    }
    storage_metrics_add(STORAGE_LOGICAL_US, esp_timer_get_time() - started);
    xSemaphoreGive(s->lock);
    return err;
}
/* Write-through only. Invalidate BEFORE forwarding, including failed/partial
 * mutations. Sync also drops cached pages so verification after persistence
 * starts with fresh media reads. No write result is inserted into this cache. */
static esp_err_t write_pages(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    invalidate(s);
    esp_err_t err = s->source->ops->write ? s->source->ops->write(s->source, src, address, size) : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t erase_pages(esp_blockdev_handle_t dev, uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    invalidate(s);
    esp_err_t err = s->source->ops->erase ? s->source->ops->erase(s->source, address, size) : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t sync_pages(esp_blockdev_handle_t dev) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    invalidate(s);
    esp_err_t err = s->source->ops->sync ? s->source->ops->sync(s->source) : ESP_OK;
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t ioctl_pages(esp_blockdev_handle_t dev, const uint8_t command, void *args) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    invalidate(s);
    esp_err_t err = s->source->ops->ioctl ? s->source->ops->ioctl(s->source, command, args) : ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t release_pages(esp_blockdev_handle_t dev) {
    storage_blockdev_t *s = dev->ctx;
    /* The VFS owner must quiesce users before releasing, as with the source. */
    esp_err_t err = s->source->ops->release(s->source);
    vSemaphoreDelete(s->lock);
    free(s->entries);
    free(s->data);
    free(s);
    return err;
}
static const esp_blockdev_ops_t ops = {
    .read = read_pages, .write = write_pages, .erase = erase_pages,
    .sync = sync_pages, .ioctl = ioctl_pages, .release = release_pages,
};
esp_err_t storage_blockdev_wrap(esp_blockdev_handle_t source, size_t cache_bytes,
                                esp_blockdev_handle_t *out) {
    if (!source || !out || !source->ops || !source->ops->read || !source->ops->release ||
        !source->geometry.read_size) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    storage_blockdev_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    s->lock = xSemaphoreCreateMutex();
    if (!s->lock) { free(s); return ESP_ERR_NO_MEM; }
    s->source = source;
    s->page_size = source->geometry.read_size;
    s->count = cache_bytes / s->page_size;
    if (s->count) {
        s->entries = heap_caps_calloc(s->count, sizeof(*s->entries), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s->data = heap_caps_malloc(s->count * s->page_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->entries || !s->data) {
            free(s->entries); free(s->data);
            s->entries = NULL; s->data = NULL; s->count = 0;
            ESP_LOGW("app_storage", "NAND read cache allocation failed; using uncached reads");
        }
    }
    s->device = *source;
    s->device.ctx = s;
    s->device.ops = &ops;
    *out = &s->device;
    ESP_LOGI("app_storage", "NAND logical read cache: page=%u slots=%u psram_bytes=%u",
             (unsigned)s->page_size, (unsigned)s->count, (unsigned)(s->count * s->page_size));
    return ESP_OK;
}
