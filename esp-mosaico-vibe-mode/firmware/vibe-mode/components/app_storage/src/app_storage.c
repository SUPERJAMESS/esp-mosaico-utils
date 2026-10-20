// SPDX-License-Identifier: Apache-2.0
#include "app_storage.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND &&                  \
    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_nand_flash.h"
#include "storage_metrics.h"
#include "storage_blockdev.h"
#include "storage_volume.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    esp_blockdev_handle_t blockdev;
    spi_device_handle_t spi;
    bool bus_initialized;
    bool mounted;
    bool busy;
    portMUX_TYPE lock;
} app_storage_context_t;
static app_storage_context_t s_storage = {.lock = portMUX_INITIALIZER_UNLOCKED};
static const char *TAG = "app_storage";
static atomic_int s_mount_status = ESP_ERR_INVALID_STATE;
#define APP_STORAGE_TRANSFER_BYTES 4096U
#define NAND_UPDATE_MOUNT_PATH APP_STORAGE_MOUNT_PATH

static esp_err_t release_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "preload NAND control GPIO%d", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure NAND control GPIO%d", pin);
    return gpio_set_level(pin, 1);
}

static void nand_cleanup_partial(app_storage_context_t *ctx)
{
    if (ctx->blockdev != NULL && !ctx->mounted) {
        (void)ctx->blockdev->ops->release(ctx->blockdev);
        ctx->blockdev = NULL;
    }
    if (ctx->spi != NULL) {
        storage_metrics_bind(NULL, 0);
        (void)spi_bus_remove_device(ctx->spi);
        ctx->spi = NULL;
    }
    if (ctx->bus_initialized) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
        ctx->bus_initialized = false;
    }
}

static esp_err_t ensure_mounted(app_storage_context_t *ctx, bool format)
{
    if (ctx->mounted) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(release_control_pin(BSP_NAND_HOLD), TAG, "release NAND HOLD");
    ESP_RETURN_ON_ERROR(release_control_pin(BSP_NAND_WP), TAG, "release NAND WP");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = APP_STORAGE_TRANSFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    ctx->bus_initialized = true;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(BSP_NAND_SPI_HOST, &device_config, &ctx->spi);
    if (err != ESP_OK) {
        nand_cleanup_partial(ctx);
        return err;
    }

    spi_nand_flash_config_t nand_config = {
        .device_handle = ctx->spi,
        .gc_factor = 0,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    storage_metrics_bind(ctx->spi, 0);
    const int64_t driver_started = esp_timer_get_time();
    err = spi_nand_flash_init_with_layers(&nand_config, &ctx->blockdev);
    if (err != ESP_OK) {
        nand_cleanup_partial(ctx);
        return err;
    }
    ESP_LOGI("boot_perf", "nand_driver_us=%lld", (long long)(esp_timer_get_time() - driver_started));
    uint64_t volume_bytes = ctx->blockdev->geometry.disk_size;
    if (!format) {
        /* Preserve existing geometry during normal mounts. Explicit recovery
         * must also work when the old superblock cannot be read. */
        err = storage_volume_size(ctx->blockdev, &volume_bytes);
        if (err != ESP_OK) {
            nand_cleanup_partial(ctx);
            return err;
        }
    }
    storage_metrics_bind(ctx->spi, ctx->blockdev->geometry.read_size);
    esp_blockdev_handle_t cached = NULL;
    err = storage_blockdev_wrap(ctx->blockdev, CONFIG_IRIS_FACTORY_NAND_READ_CACHE_KIB * 1024U, &cached);
    if (err != ESP_OK) {
        nand_cleanup_partial(ctx);
        return err;
    }
    ctx->blockdev = cached;
    /* Dhara capacity can change as its bad-block estimate is restored. Keep
     * existing geometry unless the user requested a new filesystem. */
    ctx->blockdev->geometry.disk_size = volume_bytes;

    if (format) {
        err = esp_littlefs_format_blockdev(ctx->blockdev);
        if (err != ESP_OK) {
            nand_cleanup_partial(ctx);
            return err;
        }
    }

    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = NAND_UPDATE_MOUNT_PATH,
        .blockdev = ctx->blockdev,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    const int64_t mount_started = esp_timer_get_time();
    err = esp_vfs_littlefs_register(&mount_config);
    ESP_LOGI("boot_perf", "nand_mount_us=%lld result=%d",
             (long long)(esp_timer_get_time() - mount_started), (int)err);
    if (err != ESP_OK) {
        /* LittleFS consumes and releases the block device on mount failure. */
        ctx->blockdev = NULL;
        nand_cleanup_partial(ctx);
        return err;
    }
    ctx->mounted = true;

    ESP_LOGI(TAG, "NAND LittleFS mounted read-write");
    return ESP_OK;
}

esp_err_t app_storage_mount(void)
{
    if (atomic_load(&s_mount_status) == ESP_OK) return ESP_OK;
    atomic_store(&s_mount_status, ESP_ERR_INVALID_STATE);
    esp_err_t err = ensure_mounted(&s_storage, false);
    /* INVALID_STATE is reserved for pending readiness, not a failed mount. */
    atomic_store(&s_mount_status, err == ESP_ERR_INVALID_STATE ? ESP_FAIL : err);
    return err;
}
esp_err_t app_storage_status(void)
{
    return atomic_load(&s_mount_status);
}
esp_err_t app_storage_access_status(void)
{
    esp_err_t err = app_storage_status();
    if (err != ESP_OK) return err;
    app_storage_context_t *ctx = &s_storage;
    taskENTER_CRITICAL(&ctx->lock);
    bool busy = ctx->busy;
    taskEXIT_CRITICAL(&ctx->lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t app_storage_error(void)
{
    esp_err_t status = app_storage_status();
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE && status != ESP_ERR_NOT_SUPPORTED)
        return status;
    return storage_blockdev_read_error();
}
esp_err_t app_storage_format(void)
{
    /* Caller owns the local reservation and has drained external VFS users. */
    app_storage_context_t *ctx = &s_storage;
    atomic_store(&s_mount_status, ESP_ERR_INVALID_STATE);
    esp_err_t err = ESP_OK;
    if (ctx->mounted) {
        err = esp_vfs_littlefs_unregister_blockdev(ctx->blockdev);
        if (err == ESP_OK) {
            ctx->mounted = false;
            ctx->blockdev = NULL; /* Unregister consumes the handle. */
            nand_cleanup_partial(ctx);
        }
    }
    if (err == ESP_OK) err = ensure_mounted(ctx, true);
    if (err == ESP_OK) storage_blockdev_clear_error();
    atomic_store(&s_mount_status, err == ESP_ERR_INVALID_STATE ? ESP_FAIL : err);
    return err;
}
esp_err_t app_storage_info(uint64_t *total, uint64_t *used)
{
    if (!total || !used) return ESP_ERR_INVALID_ARG;
    if (!s_storage.mounted) return ESP_ERR_INVALID_STATE;
    size_t t = 0, u = 0;
    esp_err_t err = esp_littlefs_blockdev_info(s_storage.blockdev, &t, &u);
    if (err == ESP_OK) { *total = t; *used = u; }
    return err;
}
esp_err_t app_storage_reserve(void)
{
    app_storage_context_t *ctx = &s_storage;
    taskENTER_CRITICAL(&ctx->lock);
    bool busy = ctx->busy;
    if (!busy)
        ctx->busy = true;
    taskEXIT_CRITICAL(&ctx->lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}
void app_storage_release(void)
{
    app_storage_context_t *ctx = &s_storage;
    storage_metrics_end();
    taskENTER_CRITICAL(&ctx->lock);
    ctx->busy = false;
    taskEXIT_CRITICAL(&ctx->lock);
}
esp_err_t app_storage_acquire(void)
{
    esp_err_t err = app_storage_reserve();
    if (err != ESP_OK)
        return err;
    err = app_storage_mount();
    if (err != ESP_OK)
        app_storage_release();
    else
        storage_metrics_begin();
    return err;
}
#else
esp_err_t app_storage_status(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_access_status(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_error(void) { return ESP_OK; }
esp_err_t app_storage_format(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_info(uint64_t *total, uint64_t *used)
{
    (void)total; (void)used;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_storage_mount(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_storage_reserve(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_storage_acquire(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
void app_storage_release(void) {}
#endif
