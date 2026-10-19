#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_IMAGE_INVALID 6
#define ESP_ERR_INVALID_CRC 7
#define ESP_ERR_INVALID_SIZE 8
#define ESP_ERR_NOT_FOUND 9
#define ESP_ERR_INVALID_ARG 10
#define CONFIG_PARTITION_TABLE_OFFSET 0x8000
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static void (*pending_task)(void *);
static void *pending_argument;
static int create_error, reserve_error, stage_error, commit_error, ota_active;
static int reserved, prepared, commits, aborts;
static uint8_t *staged;
static uint8_t remote[1024];
static size_t remote_size, remote_offset;
static int http_status, read_error, chunked, incomplete, opens, cleanups;
static bool device_header;
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *out)
{ if (create_error) return 0; pending_task = fn; pending_argument = arg; return pdPASS; }
static void vTaskDelay(int ticks) {}
static void vTaskDelete(void *task) {}
static void *heap_caps_malloc(size_t n, unsigned caps) { return malloc(n); }
static void esp_fill_random(void *data, size_t n) { memset(data, 0x5a, n); }
typedef struct { uint8_t data[24]; } esp_image_header_t;
typedef struct { uint32_t address, length; } esp_image_segment_header_t;
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432U
typedef struct { uint32_t magic_word, secure_version, reserved[2]; char version[32], project_name[32]; uint8_t rest[176]; } esp_app_desc_t;
typedef struct { unsigned subtype, address, size; } esp_partition_t;
#define ESP_PARTITION_SUBTYPE_APP_TEST 0x20
static esp_partition_t running = {.subtype=0x20, .address=0x20000, .size=0x1c0000};
static const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
typedef struct { bool active; } esp_iris_ota_status_t;
static int esp_iris_ota_get_status(esp_iris_ota_status_t *status) { status->active = ota_active; return ESP_OK; }
static int esp_flash_read(void *chip, void *dst, size_t off, size_t n) { memset(dst, 0, n); return ESP_OK; }
static int esp_flash_get_size(void *chip, uint32_t *n) { *n = 0x1000000; return ESP_OK; }
#define PSA_ALG_SHA_256 1
#define PSA_SUCCESS 0
static int psa_hash_compute(int alg, const void *data, size_t n, uint8_t *out, size_t capacity, size_t *length)
{ SHA256(data, n, out); *length = 32; return 0; }
static void esp_crt_bundle_attach(void) {}
typedef struct { const char *url; void (*crt_bundle_attach)(void); int timeout_ms, buffer_size, buffer_size_tx; bool disable_auto_redirect; } esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{ device_header = false; assert(!strcmp(config->url, "https://dl.espressif.com/mosaico/esp-mosaico-vibe-mode-0.2.1.bin")); assert(config->disable_auto_redirect && config->crt_bundle_attach); return (void *)&remote; }
static int esp_iris_format_device_id(char out[33])
{ strcpy(out, "4553502d495249530100001122334455"); return ESP_OK; }
static int esp_http_client_set_header(void *c, const char *key, const char *value)
{ assert(!strcmp(key, "X-Device-ID") && !strcmp(value, "4553502d495249530100001122334455")); device_header = true; return ESP_OK; }
static int esp_http_client_delete_header(void *c, const char *key)
{ device_header = false; return ESP_ERR_NOT_FOUND; }
static int esp_http_client_open(void *c, int n) { assert(device_header); ++opens; remote_offset = 0; return ESP_OK; }
static int64_t esp_http_client_fetch_headers(void *c) { return chunked ? 0 : (int64_t)remote_size; }
static int esp_http_client_get_status_code(void *c) { return http_status; }
static int esp_http_client_read(void *c, char *dst, size_t n)
{ if (read_error && remote_offset >= 73) return -1; if (n > 73) n = 73; if (n > remote_size - remote_offset) n = remote_size - remote_offset; memcpy(dst, remote + remote_offset, n); remote_offset += n; return (int)n; }
static bool esp_http_client_is_complete_data_received(void *c) { return !incomplete && remote_offset == remote_size; }
static void esp_http_client_cleanup(void *c) { ++cleanups; }
#define FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE 5
#define FACTORY_SYSTEM_UPDATE_FILENAME_BYTES 129
typedef struct { uint32_t size; } esp_iris_system_update_component_t;
static int factory_system_update_source_reserve(int owner, const uint8_t *op)
{ if (reserve_error || reserved) return ESP_ERR_INVALID_STATE; reserved = 1; return ESP_OK; }
static int factory_system_update_source_prepare(int owner, const uint8_t *data, size_t size, const uint8_t *op)
{ assert(reserved); assert(strstr((const char *)data, "\"kind\":\"recovery\"")); assert(!strstr((const char *)data, "bootloader")); ++prepared; return stage_error; }
static int factory_system_update_source_component(int owner, int index, esp_iris_system_update_component_t *c, char *file)
{ c->size = 0x1c0000; return ESP_OK; }
static int factory_system_update_source_begin_component(int owner, const esp_iris_system_update_component_t *c)
{ staged = malloc(c->size); assert(staged); return ESP_OK; }
static int factory_system_update_source_write_component(int owner, const esp_iris_system_update_component_t *c, uint32_t off, const uint8_t *data, size_t n)
{ memcpy(staged + off, data, n); return ESP_OK; }
static int factory_system_update_source_end_component(int owner, const esp_iris_system_update_component_t *c, const uint8_t *hash)
{ uint8_t actual[32]; SHA256(staged, c->size, actual); assert(!memcmp(hash, actual, 32)); return ESP_OK; }
static int factory_system_update_source_commit(int owner, const uint8_t *op)
{ assert(reserved && staged); ++commits; return commit_error; }
static void factory_system_update_source_abort(int owner, const uint8_t *op, int reason)
{ reserved = 0; ++aborts; }
