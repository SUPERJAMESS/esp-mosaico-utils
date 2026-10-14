// SPDX-License-Identifier: Apache-2.0
#include "app_storage.h"
#include "sdkconfig.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND &&                  \
    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "spi_nand_flash.h"
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
        (void)spi_bus_remove_device(ctx->spi);
        ctx->spi = NULL;
    }
    if (ctx->bus_initialized) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
        ctx->bus_initialized = false;
    }
}

static esp_err_t ensure_mounted(app_storage_context_t *ctx)
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
    err = spi_nand_flash_init_with_layers(&nand_config, &ctx->blockdev);
    if (err != ESP_OK) {
        nand_cleanup_partial(ctx);
        return err;
    }

    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = NAND_UPDATE_MOUNT_PATH,
        .blockdev = ctx->blockdev,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    err = esp_vfs_littlefs_register(&mount_config);
    if (err != ESP_OK) {
        /* LittleFS consumes and releases the block device on mount failure. */
        ctx->blockdev = NULL;
        nand_cleanup_partial(ctx);
        return err;
    }
    ctx->mounted = true;

    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_blockdev_info(ctx->blockdev, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "NAND LittleFS mounted read-write: total=%u used=%u", (unsigned)total,
                 (unsigned)used);
    }
    return ESP_OK;
}

esp_err_t app_storage_mount(void)
{
    return ensure_mounted(&s_storage);
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
    return err;
}
#else
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
