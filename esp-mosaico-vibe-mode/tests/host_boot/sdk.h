#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG,
       ESP_ERR_INVALID_STATE, ESP_ERR_NVS_NOT_FOUND };
enum { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA };
enum { ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10,
       ESP_PARTITION_SUBTYPE_APP_OTA_MAX = 0x20,
       ESP_PARTITION_SUBTYPE_APP_TEST = 0x20,
       ESP_PARTITION_SUBTYPE_ANY = 0xff, ESP_PARTITION_SUBTYPE_DATA_OTA = 0 };
enum { ESP_IMAGE_VERIFY, NVS_READONLY, NVS_READWRITE };
typedef struct { uint32_t address, size; int type, subtype; const char *label; } esp_partition_t;
typedef const esp_partition_t *esp_partition_iterator_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { int unused; } esp_image_metadata_t;
typedef int nvs_handle_t;
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_partition_iterator_t esp_partition_find(int, int, const char *);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_image_verify(int, const esp_partition_pos_t *, esp_image_metadata_t *);
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
