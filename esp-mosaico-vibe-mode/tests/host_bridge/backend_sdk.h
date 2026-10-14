#pragma once
#include "sdk.h"
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_PARTITION_SUBTYPE_DATA_PHY 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 3
#define PART_FLAG_ENCRYPTED 1
#define PART_FLAG_READONLY 2
#define ESP_PARTITION_MAGIC 0x50aa
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_CHIP_ID_ESP32S31 32
#define ESP_IMAGE_VERIFY 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_FALSE(condition, err, ...)                                       \
    do {                                                                               \
        if (!(condition))                                                              \
            return (err);                                                              \
    } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...)                                                 \
    do {                                                                               \
        esp_err_t e = (expr);                                                          \
        if (e != ESP_OK)                                                               \
            return e;                                                                  \
    } while (0)
#define ESP_GOTO_ON_FALSE(condition, err, label, ...)                                  \
    do {                                                                               \
        if (!(condition)) {                                                            \
            ret = (err);                                                               \
            goto label;                                                                \
        }                                                                              \
    } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, ...)                                            \
    do {                                                                               \
        esp_err_t e = (expr);                                                          \
        if (e != ESP_OK) {                                                             \
            ret = e;                                                                   \
            goto label;                                                                \
        }                                                                              \
    } while (0)
typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t type, subtype;
    esp_partition_pos_t pos;
    char label[16];
    uint32_t flags;
} esp_partition_info_t;
typedef struct __attribute__((packed)) {
    uint8_t magic, segment_count, spi_mode, spi_speed;
    uint32_t entry_addr;
    uint8_t wp_pin, spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full, max_chip_rev_full;
    uint8_t reserved[4], hash_appended;
} esp_image_header_t;
typedef struct {
    uint32_t load_addr, data_len;
} esp_image_segment_header_t;
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
typedef struct {
    int unused;
} esp_image_metadata_t;
extern void *esp_flash_default_chip;
esp_err_t esp_partition_table_verify(const esp_partition_info_t *entries, bool checksum,
                                     int *count);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, uint32_t offset,
                                    size_t size);
esp_err_t esp_partition_write(const esp_partition_t *p, uint32_t offset,
                              const void *data, size_t size);
esp_err_t esp_flash_set_dangerous_write_protection(void *chip, bool enabled);
esp_err_t esp_flash_erase_region(void *chip, uint32_t offset, size_t size);
esp_err_t esp_flash_write(void *chip, const void *data, uint32_t offset, size_t size);
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *pos,
                           esp_image_metadata_t *metadata);
esp_err_t esp_image_verify_bootloader(uint32_t *size);
const esp_partition_t *esp_ota_get_boot_partition(void);
esp_err_t esp_iris_crash_loop_reset(void);
esp_err_t esp_iris_mark_planned_restart(void);
