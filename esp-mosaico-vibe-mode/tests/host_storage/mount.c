// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include <string.h>
#define ESP_ERR_INVALID_STATE 0x103
#define CONFIG_ESP_IRIS_SYSTEM_UPDATE 1
#define CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND 1
#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE 1
#define CONFIG_IRIS_FACTORY_NAND_READ_CACHE_KIB 8
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { (void)(tag); int e = (call); if (e) return e; } while (0)
#define taskENTER_CRITICAL portENTER_CRITICAL
#define taskEXIT_CRITICAL portEXIT_CRITICAL
#define BIT64(n) (1ULL << (n))
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_NUM_NC -1
#define BSP_NAND_SPI_HOST 1
#define BSP_NAND_D 2
#define BSP_NAND_Q 3
#define BSP_NAND_CLK 4
#define BSP_NAND_CS 5
#define BSP_NAND_HOLD 6
#define BSP_NAND_WP 7
#define BSP_NAND_FLASH_DEFAULT_CLOCK_HZ 20000000
#define BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE 1
#define SPI_DMA_CH_AUTO 1
#define SPI_DEVICE_HALFDUPLEX 1
#define SPI_NAND_IO_MODE_SIO 1
#include "app_storage.h"
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadhd_io_num, quadwp_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size, flags; } spi_device_interface_config_t;
typedef struct { spi_device_handle_t device_handle; int gc_factor, io_mode, flags; } spi_nand_flash_config_t;
typedef struct {
    const char *base_path;
    esp_blockdev_handle_t blockdev;
    bool format_if_mount_failed, read_only, dont_mount, grow_on_mount;
} esp_vfs_littlefs_conf_t;
static unsigned mounts, usage_calls, releases;
static bool fail_mount = true, fail_format, fail_driver;
static unsigned formats, unmounts;
static int read_error;
esp_err_t storage_blockdev_read_error(void) { return read_error; }
void storage_blockdev_clear_error(void) { read_error = ESP_OK; }
static esp_err_t release_device(esp_blockdev_handle_t dev) { ++releases; return ESP_OK; }
static const esp_blockdev_ops_t ops = {.release = release_device};
static esp_blockdev_t device = {.ops = &ops, .geometry = {.read_size = 2048}};
static esp_err_t gpio_set_level(gpio_num_t pin, int level) { return ESP_OK; }
static esp_err_t gpio_config(const gpio_config_t *c) { return ESP_OK; }
static esp_err_t gpio_hold_dis(gpio_num_t pin) { return ESP_OK; }
static esp_err_t spi_bus_initialize(int bus, const spi_bus_config_t *c, int dma) { return ESP_OK; }
static esp_err_t spi_bus_free(int bus) { return ESP_OK; }
static esp_err_t spi_bus_add_device(int bus, const spi_device_interface_config_t *c, spi_device_handle_t *out) { *out = &device; return ESP_OK; }
static esp_err_t spi_bus_remove_device(spi_device_handle_t dev) { return ESP_OK; }
static esp_err_t spi_nand_flash_init_with_layers(spi_nand_flash_config_t *c, esp_blockdev_handle_t *out) { if (fail_driver) return ESP_FAIL; *out = &device; return ESP_OK; }
static esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *c) {
    ++mounts;
    assert(!strcmp(c->base_path, "/nand") && !c->format_if_mount_failed && !c->grow_on_mount);
    assert(app_storage_status() == ESP_ERR_INVALID_STATE);
    assert(app_storage_reserve() == ESP_ERR_INVALID_STATE); /* Mount owns the lease. */
    if (fail_mount) { c->blockdev->ops->release(c->blockdev); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
static esp_err_t esp_littlefs_blockdev_info(esp_blockdev_handle_t dev, size_t *total, size_t *used) {
    ++usage_calls; *total = 1024; *used = 512; return ESP_OK;
}
static esp_err_t esp_littlefs_format_blockdev(esp_blockdev_handle_t dev) {
    assert(app_storage_reserve() == ESP_ERR_INVALID_STATE);
    assert(app_storage_status() == ESP_ERR_INVALID_STATE);
    ++formats;
    if (fail_format) return ESP_FAIL;
    fail_mount = false;
    return ESP_OK;
}
static esp_err_t esp_vfs_littlefs_unregister_blockdev(esp_blockdev_handle_t dev) {
    ++unmounts;
    return dev->ops->release(dev);
}
esp_err_t storage_blockdev_wrap(esp_blockdev_handle_t dev, size_t bytes, esp_blockdev_handle_t *out) { *out = dev; return ESP_OK; }
void storage_metrics_bind(spi_device_handle_t dev, size_t size) {}
void storage_metrics_begin(void) {}
void storage_metrics_end(void) {}
#include STORAGE_SOURCE
int main(void) {
    assert(app_storage_status() == ESP_ERR_INVALID_STATE && !mounts);
    assert(app_storage_acquire() == ESP_ERR_INVALID_STATE);
    assert(app_storage_status() == ESP_FAIL && mounts == 1 && releases == 1);
    assert(!usage_calls); /* Failed mount never formats or traverses capacity. */
    assert(app_storage_error() == ESP_FAIL && !formats);
    assert(app_storage_reserve() == ESP_OK);
    assert(app_storage_format() == ESP_OK); /* Explicit recovery of unmountable FS. */
    assert(formats == 1 && mounts == 2 && !unmounts && !app_storage_error());
    app_storage_release();
    assert(app_storage_acquire() == ESP_OK); /* Failure released the reservation. */
    assert(app_storage_status() == ESP_OK && mounts == 2 && !usage_calls);
    app_storage_release();
    assert(app_storage_acquire() == ESP_OK && mounts == 2 && !usage_calls);
    uint64_t total = 0, used = 0;
    assert(app_storage_info(&total, &used) == ESP_OK && total == 1024 && used == 512);
    assert(usage_calls == 1);
    app_storage_release();
    read_error = ESP_FAIL; /* A later physical read fails on a mounted volume. */
    assert(app_storage_error() == ESP_FAIL);
    assert(app_storage_reserve() == ESP_OK);
    fail_format = true;
    assert(app_storage_format() == ESP_FAIL);
    assert(formats == 2 && unmounts == 1 && releases == 3);
    assert(app_storage_status() == ESP_FAIL && app_storage_error() == ESP_FAIL);
    app_storage_release();
    assert(app_storage_reserve() == ESP_OK);
    fail_driver = true;
    assert(app_storage_format() == ESP_FAIL && formats == 2); /* No usable chip. */
    fail_driver = fail_format = false;
    assert(app_storage_format() == ESP_OK && formats == 3);
    assert(!app_storage_error() && app_storage_status() == ESP_OK);
    app_storage_release();
    assert(app_storage_acquire() == ESP_OK);
    app_storage_release();
    return 0;
}
