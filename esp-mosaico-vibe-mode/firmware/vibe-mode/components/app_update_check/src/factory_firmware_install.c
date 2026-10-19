// SPDX-License-Identifier: Apache-2.0
#include "factory_firmware_install.h"
#include "factory_http_client.h"
#include "factory_system_update.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_BYTES 0x1c0000U
#define SLOT_OFFSET 0x20000U
#define OWNER FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE
#define DOWNLOAD_BASE "https://dl.espressif.com/mosaico/"
#define TRY(call) do { err = (call); if (err != ESP_OK) goto done; } while (0)

static const char *TAG = "vibe_firmware";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static factory_firmware_status_t s_status;
static bool s_starting;

typedef struct {
    factory_update_check_snapshot_t release;
    uint8_t operation[16];
} install_request_t;

static void progress(factory_firmware_phase_t phase, uint32_t received, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    if (s_status.phase != phase) ++s_status.revision;
    s_status.phase = phase;
    s_status.received = received;
    s_status.error = error;
    portEXIT_CRITICAL(&s_lock);
}

void factory_firmware_install_status(factory_firmware_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t digest(const void *data, size_t size, uint8_t hash[32], char hex[65])
{
    size_t length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, size, hash, 32, &length) != PSA_SUCCESS || length != 32)
        return ESP_FAIL;
    for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return ESP_OK;
}

static esp_err_t download(const factory_update_check_snapshot_t *release, uint8_t *image)
{
    char url[sizeof(DOWNLOAD_BASE) + sizeof(release->file)];
    snprintf(url, sizeof(url), "%s%s", DOWNLOAD_BASE, release->file);
    const esp_http_client_config_t config = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, .disable_auto_redirect = true,
        .buffer_size = 4096, .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = factory_http_client_set_common_headers(client);
    if (err == ESP_OK) err = esp_http_client_open(client, 0);
    uint32_t received = 0;
    if (err != ESP_OK) goto done;
    int64_t length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || length < 0 ||
        (length > 0 && length != release->size)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    const int64_t deadline = esp_timer_get_time() + 180LL * 1000000;
    while (received < release->size) {
        if (esp_timer_get_time() > deadline) { err = ESP_ERR_TIMEOUT; goto done; }
        size_t count = release->size - received;
        if (count > 4096) count = 4096;
        int n = esp_http_client_read(client, (char *)image + received, count);
        if (n <= 0) { err = ESP_ERR_INVALID_RESPONSE; goto done; }
        received += (uint32_t)n;
        progress(FIRMWARE_DOWNLOADING, received, ESP_OK);
    }
    /* Reject oversized and incomplete chunked bodies, even if the prefix hashes. */
    char extra;
    if (esp_http_client_read(client, &extra, 1) != 0 || !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_INVALID_RESPONSE;
done:
    if (err != ESP_OK) ESP_LOGE(TAG, "Download failed: %s (HTTP %d, %lu/%lu bytes)",
        esp_err_to_name(err), esp_http_client_get_status_code(client), (unsigned long)received, (unsigned long)release->size);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t verify_release(const uint8_t *image, const factory_update_check_snapshot_t *release)
{
    const size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (release->size < offset + sizeof(esp_app_desc_t)) return ESP_ERR_IMAGE_INVALID;
    esp_app_desc_t description;
    memcpy(&description, image + offset, sizeof(description));
    if (description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !memchr(description.version, 0, sizeof(description.version)) ||
        !memchr(description.project_name, 0, sizeof(description.project_name)) ||
        strcmp(description.project_name, "vibe-mode") || strcmp(description.version, release->version))
        return ESP_ERR_IMAGE_INVALID;
    uint8_t hash[32];
    char hex[65];
    esp_err_t err = digest(image, release->size, hash, hex);
    return err != ESP_OK ? err : strcmp(hex, release->sha256) ? ESP_ERR_INVALID_CRC : ESP_OK;
}

static esp_err_t stage_image(const install_request_t *request, uint8_t *image)
{
    esp_err_t err = ESP_OK;
    uint8_t hash[32], layout_hash[32];
    char hex[65], layout_hex[65], manifest[768];
    uint8_t *table = malloc(4096);
    if (!table) return ESP_ERR_NO_MEM;
    TRY(esp_flash_read(NULL, table, CONFIG_PARTITION_TABLE_OFFSET, 4096));
    TRY(digest(table, 4096, layout_hash, layout_hex));
    TRY(digest(image, SLOT_BYTES, hash, hex));
    uint32_t flash_size = 0;
    TRY(esp_flash_get_size(NULL, &flash_size));
    /* Bind the fixed-slot transaction to the installed layout; never replace it. */
    int length = snprintf(manifest, sizeof(manifest),
        "{\"schema\":\"esp-iris-system-update/0.2\",\"target\":{\"chip_id\":32,\"flash_size\":%lu},"
        "\"target_layout_sha256\":\"%s\",\"components\":[{\"id\":1,\"kind\":\"recovery\","
        "\"target_offset\":%u,\"size\":%u,\"file\":\"vibe-mode.bin\",\"sha256\":\"%s\"}]}",
        (unsigned long)flash_size, layout_hex, SLOT_OFFSET, SLOT_BYTES, hex);
    if (length < 0 || (size_t)length >= sizeof(manifest)) { err = ESP_ERR_INVALID_SIZE; goto done; }
    TRY(factory_system_update_source_prepare(OWNER, (const uint8_t *)manifest, length, request->operation));
    esp_iris_system_update_component_t component;
    char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES];
    TRY(factory_system_update_source_component(OWNER, 0, &component, filename));
    TRY(factory_system_update_source_begin_component(OWNER, &component));
    for (uint32_t offset = 0; offset < SLOT_BYTES; offset += 4096)
        TRY(factory_system_update_source_write_component(OWNER, &component, offset, image + offset, 4096));
    TRY(factory_system_update_source_end_component(OWNER, &component, hash));
done:
    free(table);
    return err;
}

static void install_task(void *argument)
{
    install_request_t *request = argument;
    uint8_t *image = NULL;
    bool reserved = false;
    esp_err_t err = factory_system_update_source_reserve(OWNER, request->operation);
    if (err != ESP_OK) goto done;
    reserved = true;
    esp_iris_ota_status_t ota = {0};
    err = esp_iris_ota_get_status(&ota);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) err = ESP_OK;
    if (err == ESP_OK && ota.active) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) goto done;
    progress(FIRMWARE_DOWNLOADING, 0, ESP_OK);
    image = heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    err = ESP_ERR_NO_MEM;
    if (!image) goto done;
    memset(image, 0xff, SLOT_BYTES);
    TRY(download(&request->release, image));
    progress(FIRMWARE_VERIFYING, request->release.size, ESP_OK);
    TRY(verify_release(image, &request->release));
    TRY(stage_image(request, image));
    free(image);
    image = NULL;
    progress(FIRMWARE_WRITING, request->release.size, ESP_OK);
    /* Keep the power warning painted before entering the destructive commit. */
    vTaskDelay(pdMS_TO_TICKS(300));
    TRY(factory_system_update_source_commit(OWNER, request->operation));
    progress(FIRMWARE_RESTARTING, request->release.size, ESP_OK);
done:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware update failed: %s", esp_err_to_name(err));
        if (reserved) factory_system_update_source_abort(OWNER, request->operation, err);
        factory_firmware_status_t status;
        factory_firmware_install_status(&status);
        progress(FIRMWARE_FAILED, status.received, err);
    } else ESP_LOGI(TAG, "Firmware verified and committed; restarting");
    free(image);
    free(request);
    vTaskDelete(NULL);
}

esp_err_t factory_firmware_install_start(const factory_update_check_snapshot_t *release)
{
    if (!release || !release->available || !release->installable || release->size > SLOT_BYTES) return ESP_ERR_NOT_SUPPORTED;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_TEST || running->address != SLOT_OFFSET || running->size != SLOT_BYTES) {
        ESP_LOGE(TAG, "Update rejected: running slot subtype=0x%x offset=0x%lx size=0x%lx",
                 running ? running->subtype : 0xff, running ? (unsigned long)running->address : 0,
                 running ? (unsigned long)running->size : 0);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    bool busy = s_starting || (s_status.phase != FIRMWARE_IDLE && s_status.phase != FIRMWARE_FAILED);
    if (!busy) s_starting = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        ESP_LOGE(TAG, "Update rejected: firmware installer is busy");
        return ESP_ERR_INVALID_STATE;
    }
    install_request_t *request = malloc(sizeof(*request));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!request) goto done;
    request->release = *release;
    esp_fill_random(request->operation, sizeof(request->operation));
    request->operation[0] |= 1;
    portENTER_CRITICAL(&s_lock);
    s_status.total = release->size;
    portEXIT_CRITICAL(&s_lock);
    progress(FIRMWARE_PREPARING, 0, ESP_OK);
    if (xTaskCreate(install_task, "vibe_install", 20 * 1024, request, 4, NULL) == pdPASS) {
        request = NULL;
        err = ESP_OK;
    } else {
        err = ESP_ERR_NO_MEM;
        progress(FIRMWARE_FAILED, 0, err);
        ESP_LOGE(TAG, "Failed to create firmware installer task");
    }
done:
    free(request);
    portENTER_CRITICAL(&s_lock);
    s_starting = false;
    portEXIT_CRITICAL(&s_lock);
    return err;
}
