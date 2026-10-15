// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EXT_RAM_BSS_ATTR
#define FACTORY_SYSTEM_METADATA_PARTITION "sysmeta"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_STR 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
typedef unsigned nvs_handle_t;
typedef int nvs_open_mode_t;
typedef unsigned *nvs_iterator_t;
typedef struct { char key[16]; } nvs_entry_info_t;
typedef bool *SemaphoreHandle_t;
typedef int BaseType_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
BaseType_t xTaskCreate(void (*task)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
esp_err_t nvs_flash_init_partition(const char *partition);
esp_err_t nvs_open_from_partition(const char *, const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_entry_find(const char *, const char *, int, nvs_iterator_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
void nvs_entry_info(nvs_iterator_t, nvs_entry_info_t *);
void nvs_release_iterator(nvs_iterator_t);
esp_err_t factory_network_suspend(bool);
esp_err_t factory_network_forget(void);
esp_err_t mosaico_boot_request_recovery(void);
esp_err_t factory_system_update_read_acquire(void);
void factory_system_update_read_release(void);
esp_err_t app_storage_reserve(void);
void app_storage_release(void);
void esp_restart(void);

void factory_locale_use(unsigned language);
