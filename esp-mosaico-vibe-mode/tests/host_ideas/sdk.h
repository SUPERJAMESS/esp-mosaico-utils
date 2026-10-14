// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <assert.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define EXT_RAM_BSS_ATTR
#define CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL "https://ideas.test"
#define CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID "test-s31"
#define CONFIG_IRIS_FACTORY_IDEAS_SERVER_URL "https://ideas.test"
#define CONFIG_IRIS_FACTORY_IDEAS_CA_FILE ""
#define ESP_ERR_INVALID_VERSION 0x10a
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define tskNO_AFFINITY -1
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
uint32_t esp_random(void);
#define portMAX_DELAY UINT32_MAX
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
#define PSA_HASH_OPERATION_INIT                                                                    \
    { 0 }
#define esp_flash_default_chip NULL

typedef struct mock_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout);
int xSemaphoreGive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
typedef struct mock_queue *QueueHandle_t;
typedef struct mock_task *TaskHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned timeout);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned timeout);
void vQueueDelete(QueueHandle_t queue);
int xTaskCreatePinnedToCoreWithCaps(void (*task)(void *), const char *name, unsigned stack,
                                  void *arg, unsigned priority, TaskHandle_t *handle,
                                  int core, unsigned caps);
void vTaskDelay(unsigned ticks);
int64_t esp_timer_get_time(void);
void *heap_caps_malloc(size_t size, unsigned caps);
const char *esp_err_to_name(esp_err_t err);
void esp_restart(void);

typedef struct mock_http *esp_http_client_handle_t;
#define HTTP_EVENT_ON_HEADER 1
typedef struct {
    int event_id;
    char *header_key, *header_value;
} esp_http_client_event_t;
typedef struct {
    char version[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
typedef struct {
    const char *url;
    unsigned timeout_ms, buffer_size;
    bool disable_auto_redirect;
    int method;
    esp_err_t (*crt_bundle_attach)(void *);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
} esp_http_client_config_t;
esp_err_t esp_crt_bundle_attach(void *config);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key,
                                     const char *value);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length);
int esp_http_client_write(esp_http_client_handle_t client, const char *data, int size);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

typedef SHA256_CTX psa_hash_operation_t;
int psa_crypto_init(void);
int psa_hash_setup(psa_hash_operation_t *hash, int algorithm);
int psa_hash_update(psa_hash_operation_t *hash, const uint8_t *data, size_t size);
int psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *size);
int psa_hash_abort(psa_hash_operation_t *hash);
int psa_hash_compute(int algorithm, const uint8_t *data, size_t size, uint8_t *out, size_t capacity,
                     size_t *written);

typedef unsigned nvs_handle_t;
typedef struct {
    uint32_t address;
} esp_partition_t;
esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode,
                                  nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
esp_err_t esp_flash_get_size(void *chip, uint32_t *size);
esp_err_t esp_flash_read(void *chip, void *out, uint32_t offset, size_t size);
const esp_partition_t *esp_ota_get_boot_partition(void);

/* Test controls model only component/SDK edges. Production private structures
 * are never included by the scenario driver or these fakes. */
typedef struct {
    const char *fixture_dir, *mount_dir, *release;
    const char *cancel_on_read, *fault_file;
    enum {
        MOCK_OK,
        MOCK_CORRUPT,
        MOCK_TRUNCATE,
        MOCK_NOT_FOUND,
        MOCK_REDIRECT,
        MOCK_READ_ERROR,
        MOCK_WRONG_LENGTH,
        MOCK_SERVICE_UNAVAILABLE,
        MOCK_ETAG
    } fault;
    bool network, lease, storage_busy, fail_send, fail_commit, deny_new_tasks;
    unsigned fail_init_step, tasks_created, tasks_entered, jobs_finished, mutex_live, queue_live;
    unsigned writes, resolves, assets, firmware, http_live, installs, commits, restarts;
    unsigned cancels, requests_with_identity, fault_limit, fault_hits, firmware_reads, read_chunk;
    uint64_t flash_read_bytes;
    char flash_version;
    void (*observe)(void);
} mock_environment_t;
mock_environment_t *mock_environment(void);
void mock_run_worker(void);
void mock_run_one_job(void);
void mock_stop_worker(void);
void mock_assert_idle(void);
void mock_corrupt_flash(void);
