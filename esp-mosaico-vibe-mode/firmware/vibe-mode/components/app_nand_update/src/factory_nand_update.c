#include "esp_attr.h"
// SPDX-License-Identifier: Apache-2.0

#include "factory_system_update.h"
#include "factory_nand_update.h"
#include "app_storage.h"
#include "factory_io_metrics.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"

#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND &&                  \
    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE

#include "cJSON.h"
#include "esp_check.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "mosaico_boot.h"

#define NAND_UPDATE_SERVICE_ID 0x1201U
#define NAND_UPDATE_START_METHOD_ID 2U
#define NAND_UPDATE_READ_BYTES 4096U
#define NAND_UPDATE_MOUNT_PATH "/nand"
#define NAND_FILE_VOLUME_ID "nand"
#define NAND_UPDATE_CATALOG_PATH NAND_UPDATE_MOUNT_PATH "/system-update"
#define NAND_UPDATE_MANIFEST_NAME "manifest.json"
#define NAND_UPDATE_MAX_DIRECTORY_ENTRIES 64U
#define NAND_UPDATE_LOG_STEP_PERCENT 10U

typedef struct nand_update_context nand_update_context_t;
typedef struct {
    nand_update_context_t *owner;
    char manifest_path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
} nand_update_task_context_t;

struct nand_update_context {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;
    factory_nand_update_snapshot_t *snapshot;
};
static EXT_RAM_BSS_ATTR factory_nand_update_snapshot_t s_snapshot_storage;
static nand_update_context_t s_catalog = {.snapshot = &s_snapshot_storage};
static const char *TAG = "factory_nand_update";

static void snapshot_lock(nand_update_context_t *ctx)
{
    if (ctx->mutex != NULL) {
        (void)xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    }
}

static void snapshot_unlock(nand_update_context_t *ctx)
{
    if (ctx->mutex != NULL) {
        (void)xSemaphoreGive(ctx->mutex);
    }
}

static void update_source_status(nand_update_context_t *ctx, factory_nand_update_state_t state,
                                 esp_err_t result)
{
    snapshot_lock(ctx);
    ctx->snapshot->update_state = state;
    ctx->snapshot->update_result = result;
    ++ctx->snapshot->generation;
    snapshot_unlock(ctx);
}

static bool manifest_path_valid(const char *path)
{
    if (path == NULL) {
        return false;
    }
    const size_t size = strnlen(path, FACTORY_SYSTEM_UPDATE_PATH_BYTES);
    const size_t prefix_size = strlen(NAND_UPDATE_MOUNT_PATH "/");
    if (size <= prefix_size || size >= FACTORY_SYSTEM_UPDATE_PATH_BYTES ||
        strncmp(path, NAND_UPDATE_MOUNT_PATH "/", prefix_size) != 0 || path[size - 1U] == '/' ||
        strchr(path, '\\') != NULL) {
        return false;
    }
    const char *segment = path + prefix_size;
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        const size_t segment_size = slash != NULL ? (size_t)(slash - segment) : strlen(segment);
        if (segment_size == 0 || (segment_size == 1 && segment[0] == '.') ||
            (segment_size == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        if (slash == NULL) {
            break;
        }
        segment = slash + 1;
    }
    return true;
}

static esp_err_t read_manifest(const char *path, uint8_t **manifest, size_t *manifest_size)
{
    struct stat info;
    ESP_RETURN_ON_FALSE(stat(path, &info) == 0 && S_ISREG(info.st_mode), ESP_ERR_NOT_FOUND, TAG,
                        "NAND manifest not found");
    ESP_RETURN_ON_FALSE(info.st_size > 0 &&
                            (uint64_t)info.st_size <= CONFIG_ESP_IRIS_SYSTEM_UPDATE_MANIFEST_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG, "NAND manifest size is invalid");

    const size_t size = (size_t)info.st_size;
    uint8_t *data = malloc(size);
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "allocate NAND manifest");
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        free(data);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t received = fread(data, 1, size, file);
    const bool complete = received == size && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!complete) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    *manifest = data;
    *manifest_size = size;
    return ESP_OK;
}

static esp_err_t component_path(const char *manifest_path, const char *filename,
                                char output[FACTORY_SYSTEM_UPDATE_PATH_BYTES])
{
    const char *slash = strrchr(manifest_path, '/');
    ESP_RETURN_ON_FALSE(slash != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "NAND manifest path has no directory");
    const size_t directory_size = (size_t)(slash - manifest_path) + 1U;
    const size_t filename_size = strlen(filename);
    ESP_RETURN_ON_FALSE(directory_size + filename_size < FACTORY_SYSTEM_UPDATE_PATH_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG, "NAND component path is too long");
    memcpy(output, manifest_path, directory_size);
    memcpy(output + directory_size, filename, filename_size + 1U);
    return ESP_OK;
}

static bool component_filename_valid(const char *filename)
{
    if (filename == NULL) {
        return false;
    }
    const size_t size = strnlen(filename, FACTORY_SYSTEM_UPDATE_FILENAME_BYTES);
    return size > 0 && size < FACTORY_SYSTEM_UPDATE_FILENAME_BYTES && strcmp(filename, ".") != 0 &&
           strcmp(filename, "..") != 0 && strchr(filename, '/') == NULL &&
           strchr(filename, '\\') == NULL;
}

static esp_err_t inspect_manifest_candidate(const char *manifest_path,
                                            factory_nand_update_candidate_t *candidate)
{
    uint8_t *manifest = NULL;
    size_t manifest_size = 0;
    ESP_RETURN_ON_FALSE(candidate != NULL, ESP_ERR_INVALID_ARG, TAG, "candidate output is null");
    ESP_RETURN_ON_ERROR(read_manifest(manifest_path, &manifest, &manifest_size), TAG,
                        "read NAND catalog manifest");

    cJSON *root = cJSON_ParseWithLength((const char *)manifest, manifest_size);
    free(manifest);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "parse NAND catalog manifest");

    esp_err_t result = ESP_OK;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *release = cJSON_GetObjectItemCaseSensitive(root, "release");
    const cJSON *components = cJSON_GetObjectItemCaseSensitive(root, "components");
    if (!cJSON_IsString(schema) || schema->valuestring == NULL ||
        strcmp(schema->valuestring, "esp-iris-system-update/0.2") != 0 ||
        !cJSON_IsArray(components)) {
        result = ESP_ERR_INVALID_ARG;
        goto done;
    }
    const int component_count = cJSON_GetArraySize(components);
    if (component_count <= 0 || component_count > CONFIG_ESP_IRIS_SYSTEM_UPDATE_MAX_COMPONENTS) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    memset(candidate, 0, sizeof(*candidate));
    strlcpy(candidate->manifest_path, manifest_path, sizeof(candidate->manifest_path));
    if (cJSON_IsString(release) && release->valuestring != NULL &&
        release->valuestring[0] != '\0') {
        strlcpy(candidate->release, release->valuestring, sizeof(candidate->release));
    } else {
        const char *directory_end = strrchr(manifest_path, '/');
        const char *directory_start = directory_end;
        while (directory_start != NULL && directory_start > manifest_path &&
               directory_start[-1] != '/') {
            --directory_start;
        }
        if (directory_start != NULL && directory_end > directory_start) {
            const size_t directory_size = (size_t)(directory_end - directory_start);
            const size_t copy_size = directory_size < sizeof(candidate->release) - 1U
                                         ? directory_size
                                         : sizeof(candidate->release) - 1U;
            memcpy(candidate->release, directory_start, copy_size);
            candidate->release[copy_size] = '\0';
        } else {
            strlcpy(candidate->release, "NAND bundle", sizeof(candidate->release));
        }
    }

    for (int i = 0; i < component_count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(components, i);
        if (!cJSON_IsObject(item)) {
            result = ESP_ERR_INVALID_ARG;
            goto done;
        }
        const cJSON *file = cJSON_GetObjectItemCaseSensitive(item, "file");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
        if (!cJSON_IsString(file) || file->valuestring == NULL ||
            !component_filename_valid(file->valuestring) || !cJSON_IsNumber(size) ||
            size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
            (double)(uint32_t)size->valuedouble != size->valuedouble) {
            result = ESP_ERR_INVALID_ARG;
            goto done;
        }
        char path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
        result = component_path(manifest_path, file->valuestring, path);
        if (result != ESP_OK) {
            goto done;
        }
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            result = ESP_ERR_NOT_FOUND;
            goto done;
        }
        if ((uint64_t)info.st_size != (uint32_t)size->valuedouble) {
            result = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        candidate->total_size += (uint32_t)size->valuedouble;
    }
    candidate->component_count = (uint8_t)component_count;

done:
    cJSON_Delete(root);
    return result;
}

static int compare_candidates(const void *left, const void *right)
{
    const factory_nand_update_candidate_t *a = left;
    const factory_nand_update_candidate_t *b = right;
    return strcmp(a->manifest_path, b->manifest_path);
}

static void catalog_try_add(factory_nand_update_snapshot_t *snapshot, const char *manifest_path)
{
    struct stat info;
    if (stat(manifest_path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return;
    }
    factory_nand_update_candidate_t candidate;
    if (inspect_manifest_candidate(manifest_path, &candidate) != ESP_OK) {
        ++snapshot->invalid_count;
        return;
    }
    if (snapshot->candidate_count >= FACTORY_NAND_UPDATE_MAX_CANDIDATES) {
        ++snapshot->invalid_count;
        return;
    }
    snapshot->candidates[snapshot->candidate_count++] = candidate;
}

static esp_err_t scan_catalog(factory_nand_update_snapshot_t *snapshot)
{
    struct stat root_info;
    if (stat(NAND_UPDATE_CATALOG_PATH, &root_info) != 0) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "NAND system-update catalog is empty");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "stat NAND system-update catalog failed: errno=%d", errno);
        return ESP_FAIL;
    }
    ESP_RETURN_ON_FALSE(S_ISDIR(root_info.st_mode), ESP_ERR_INVALID_STATE, TAG,
                        "NAND system-update catalog is not a directory");

    char manifest_path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
    int written = snprintf(manifest_path, sizeof(manifest_path), "%s/%s", NAND_UPDATE_CATALOG_PATH,
                           NAND_UPDATE_MANIFEST_NAME);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(manifest_path),
                        ESP_ERR_INVALID_SIZE, TAG, "NAND catalog manifest path is too long");
    catalog_try_add(snapshot, manifest_path);

    DIR *directory = opendir(NAND_UPDATE_CATALOG_PATH);
    ESP_RETURN_ON_FALSE(directory != NULL, ESP_FAIL, TAG, "open NAND system-update directory");
    struct dirent *entry;
    size_t entry_count = 0;
    while (entry_count < NAND_UPDATE_MAX_DIRECTORY_ENTRIES &&
           (entry = readdir(directory)) != NULL) {
        ++entry_count;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strchr(entry->d_name, '/') != NULL || strchr(entry->d_name, '\\') != NULL) {
            continue;
        }
        char directory_path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
        written = snprintf(directory_path, sizeof(directory_path), "%s/%s",
                           NAND_UPDATE_CATALOG_PATH, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(directory_path)) {
            ++snapshot->invalid_count;
            continue;
        }
        struct stat info;
        if (stat(directory_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
            continue;
        }
        written = snprintf(manifest_path, sizeof(manifest_path), "%s/%s", directory_path,
                           NAND_UPDATE_MANIFEST_NAME);
        if (written <= 0 || (size_t)written >= sizeof(manifest_path)) {
            ++snapshot->invalid_count;
            continue;
        }
        catalog_try_add(snapshot, manifest_path);
    }
    closedir(directory);
    qsort(snapshot->candidates, snapshot->candidate_count, sizeof(snapshot->candidates[0]),
          compare_candidates);
    return ESP_OK;
}

static void catalog_scan_task(void *argument)
{
    nand_update_context_t *ctx = argument;

    factory_nand_update_snapshot_t result = {
        .scan_state = FACTORY_NAND_SCAN_READY,
        .scan_result = ESP_OK,
        .update_state = FACTORY_NAND_UPDATE_IDLE,
        .update_result = ESP_OK,
    };
    esp_err_t err = app_storage_mount();
    if (err == ESP_OK) {
        err = scan_catalog(&result);
    }
    if (err != ESP_OK) {
        result.scan_state = FACTORY_NAND_SCAN_FAILED;
        result.scan_result = err;
    }

    snapshot_lock(ctx);
    result.generation = ctx->snapshot->generation + 1U;
    (*ctx->snapshot) = result;
    snapshot_unlock(ctx);
    app_storage_release();
    vTaskDelete(NULL);
}

static esp_err_t read_component(const char *path,
                                const esp_iris_system_update_component_t *component)
{
    struct stat info;
    ESP_RETURN_ON_FALSE(stat(path, &info) == 0 && S_ISREG(info.st_mode), ESP_ERR_NOT_FOUND, TAG,
                        "NAND component not found");
    ESP_RETURN_ON_FALSE((uint64_t)info.st_size == component->size, ESP_ERR_INVALID_SIZE, TAG,
                        "NAND component size mismatch");

    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_ERR_NOT_FOUND, TAG, "open NAND component");
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    /* SPI1 NOR writes from PSRAM otherwise bounce through IDF's tiny temporary
     * buffer. Reserve the internal chunk before any destructive erase. */
    uint8_t *buffer = heap_caps_malloc(NAND_UPDATE_READ_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = buffer ? ESP_OK : ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool hash_active = false;
    if (err == ESP_OK) {
        err = psa_crypto_init() == PSA_SUCCESS &&
                      psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS
                  ? ESP_OK : ESP_FAIL;
        hash_active = err == ESP_OK;
    }
    int64_t started = factory_io_now();
    if (err == ESP_OK)
        err = factory_system_update_source_begin_component(FACTORY_SYSTEM_UPDATE_OWNER_NAND, component);
    metrics.begin = factory_io_now() - started;
    uint32_t received = 0;
    uint32_t next_log_percent = NAND_UPDATE_LOG_STEP_PERCENT;
    while (err == ESP_OK && received < component->size) {
        size_t request = component->size - received;
        if (request > NAND_UPDATE_READ_BYTES) {
            request = NAND_UPDATE_READ_BYTES;
        }
        started = factory_io_now();
        const size_t read_size = fread(buffer, 1, request, file);
        metrics.read += factory_io_now() - started;
        metrics.bytes += read_size;
        if (read_size == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        started = factory_io_now();
        psa_status_t status = psa_hash_update(&hash, buffer, read_size);
        metrics.hash += factory_io_now() - started;
        if (status != PSA_SUCCESS) {
            err = ESP_FAIL;
            break;
        }
        started = factory_io_now();
        err = factory_system_update_source_write_component(FACTORY_SYSTEM_UPDATE_OWNER_NAND,
                                                           component, received, buffer, read_size);
        metrics.write += factory_io_now() - started;
        if (err == ESP_OK) {
            received += (uint32_t)read_size;
            const uint32_t percent = (uint32_t)(((uint64_t)received * 100U) / component->size);
            if (received == component->size || percent >= next_log_percent) {
                ESP_LOGI(TAG,
                         "NAND component %u progress: %" PRIu32 "/%" PRIu32 " bytes (%" PRIu32
                         "%%)",
                         component->id, received, component->size, percent);
                next_log_percent =
                    ((percent / NAND_UPDATE_LOG_STEP_PERCENT) + 1U) * NAND_UPDATE_LOG_STEP_PERCENT;
            }
        }
    }

    uint8_t digest[ESP_IRIS_SYSTEM_SHA256_BYTES] = {0};
    size_t digest_size = 0;
    if (err == ESP_OK && (received != component->size || fgetc(file) != EOF || ferror(file))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        const psa_status_t status = psa_hash_finish(&hash, digest, sizeof(digest), &digest_size);
        if (status == PSA_SUCCESS) {
            hash_active = false;
        }
        if (status != PSA_SUCCESS || digest_size != sizeof(digest)) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "finalizing NAND component %u", component->id);
        started = factory_io_now();
        err = factory_system_update_source_end_component(FACTORY_SYSTEM_UPDATE_OWNER_NAND,
                                                         component, digest);
        metrics.end = factory_io_now() - started;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NAND component %u verified", component->id);
        }
    }
    if (hash_active) {
        (void)psa_hash_abort(&hash);
    }
    factory_io_report("nand_install", path, &metrics, buffer, NAND_UPDATE_READ_BYTES, err);
    free(buffer);
    fclose(file);
    return err;
}

static void generate_operation_id(uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    esp_fill_random(operation_id, ESP_IRIS_SYSTEM_OPERATION_ID_BYTES);
    bool nonzero = false;
    for (size_t i = 0; i < ESP_IRIS_SYSTEM_OPERATION_ID_BYTES; ++i) {
        nonzero |= operation_id[i] != 0;
    }
    if (!nonzero) {
        operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES - 1U] = 1;
    }
}

esp_err_t factory_nand_install_idea(const char *manifest_path, const char *manifest,
                                    esp_err_t (*before_commit)(void *), void *ctx)
{
    if (!manifest_path || !manifest)
        return ESP_ERR_INVALID_ARG;
    uint8_t operation[16];
    generate_operation_id(operation);
    esp_err_t err = factory_system_update_source_reserve_idea(operation);
    if (err != ESP_OK)
        return err;
    err = factory_system_update_source_prepare(
        FACTORY_SYSTEM_UPDATE_OWNER_NAND, (const uint8_t *)manifest, strlen(manifest), operation);
    /* A failed/interrupted single-slot install must return to Vibe Mode. */
    if (err == ESP_OK)
        err = mosaico_boot_request_recovery();
    size_t count = factory_system_update_source_component_count(FACTORY_SYSTEM_UPDATE_OWNER_NAND);
    for (size_t i = 0; err == ESP_OK && i < count; ++i) {
        esp_iris_system_update_component_t component;
        char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES], path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
        err = factory_system_update_source_component(FACTORY_SYSTEM_UPDATE_OWNER_NAND, i,
                                                     &component, filename);
        if (err == ESP_OK)
            err = component_path(manifest_path, filename, path);
        if (err == ESP_OK)
            err = read_component(path, &component);
    }
    if (err == ESP_OK && before_commit)
        err = before_commit(ctx);
    if (err == ESP_OK)
        err = factory_system_update_source_commit_deferred(operation);
    if (err != ESP_OK) {
        (void)mosaico_boot_request_recovery();
        factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_NAND, operation, err);
    }
    return err;
}

static void nand_update_task(void *argument)
{
    nand_update_context_t *ctx = ((nand_update_task_context_t *)argument)->owner;
    nand_update_task_context_t *context = argument;
    bool prepared = false;
    uint8_t *manifest = NULL;
    size_t manifest_size = 0;

    esp_err_t err = app_storage_mount();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "reading system-update manifest from NAND");
        err = read_manifest(context->manifest_path, &manifest, &manifest_size);
    }
    if (err == ESP_OK) {
        err = factory_system_update_source_prepare(FACTORY_SYSTEM_UPDATE_OWNER_NAND, manifest,
                                                   manifest_size, context->operation_id);
        prepared = err == ESP_OK;
        if (prepared) {
            update_source_status(ctx, FACTORY_NAND_UPDATE_RUNNING, ESP_OK);
        }
    }
    free(manifest);

    const size_t component_count =
        prepared ? factory_system_update_source_component_count(FACTORY_SYSTEM_UPDATE_OWNER_NAND)
                 : 0;
    for (size_t i = 0; err == ESP_OK && i < component_count; ++i) {
        esp_iris_system_update_component_t component;
        char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES];
        char path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
        err = factory_system_update_source_component(FACTORY_SYSTEM_UPDATE_OWNER_NAND, i,
                                                     &component, filename);
        if (err == ESP_OK) {
            err = component_path(context->manifest_path, filename, path);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "reading NAND component %u (%lu bytes)", component.id,
                     (unsigned long)component.size);
            err = read_component(path, &component);
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "all NAND components verified; starting commit");
        err = factory_system_update_source_commit(FACTORY_SYSTEM_UPDATE_OWNER_NAND,
                                                  context->operation_id);
    }
    if (err != ESP_OK) {
        factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_NAND, context->operation_id,
                                           err);
        update_source_status(ctx, FACTORY_NAND_UPDATE_FAILED, err);
        ESP_LOGE(TAG, "NAND system update failed: %s", esp_err_to_name(err));
    }

    free(context);
    app_storage_release();
    vTaskDelete(NULL);
}

esp_err_t factory_system_update_start_nand(const char *manifest_path)
{
    nand_update_context_t *ctx = &s_catalog;
    ESP_RETURN_ON_FALSE(manifest_path_valid(manifest_path), ESP_ERR_INVALID_ARG, TAG,
                        "invalid NAND system-update manifest path");
    nand_update_task_context_t *context = calloc(1, sizeof(*context));
    ESP_RETURN_ON_FALSE(context != NULL, ESP_ERR_NO_MEM, TAG, "allocate NAND update task");
    context->owner = ctx;
    strlcpy(context->manifest_path, manifest_path, sizeof(context->manifest_path));
    generate_operation_id(context->operation_id);

    if (app_storage_reserve() != ESP_OK) {
        free(context);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t reserve_err = factory_system_update_source_reserve(
        FACTORY_SYSTEM_UPDATE_OWNER_NAND, context->operation_id);
    if (reserve_err != ESP_OK) {
        app_storage_release();
        free(context);
        return reserve_err;
    }
    update_source_status(ctx, FACTORY_NAND_UPDATE_STARTING, ESP_OK);
    if (xTaskCreate(nand_update_task, "nand_sysupdate",
                    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_TASK_STACK, context, 4,
                    NULL) != pdPASS) {
        app_storage_release();
        factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_NAND, context->operation_id,
                                           ESP_ERR_NO_MEM);
        update_source_status(ctx, FACTORY_NAND_UPDATE_FAILED, ESP_ERR_NO_MEM);
        free(context);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t factory_nand_update_request_scan(void)
{
    nand_update_context_t *ctx = &s_catalog;
    ESP_RETURN_ON_FALSE(ctx->mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "NAND catalog is not initialized");
    ESP_RETURN_ON_FALSE(app_storage_reserve() == ESP_OK, ESP_ERR_INVALID_STATE, TAG,
                        "NAND update source is busy");

    snapshot_lock(ctx);
    ctx->snapshot->scan_state = FACTORY_NAND_SCAN_RUNNING;
    ctx->snapshot->scan_result = ESP_OK;
    ctx->snapshot->update_state = FACTORY_NAND_UPDATE_IDLE;
    ctx->snapshot->update_result = ESP_OK;
    ctx->snapshot->candidate_count = 0;
    ctx->snapshot->invalid_count = 0;
    memset(ctx->snapshot->candidates, 0, sizeof(ctx->snapshot->candidates));
    ++ctx->snapshot->generation;
    snapshot_unlock(ctx);

    if (xTaskCreate(catalog_scan_task, "nand_catalog",
                    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_TASK_STACK, ctx, 3, NULL) != pdPASS) {
        snapshot_lock(ctx);
        ctx->snapshot->scan_state = FACTORY_NAND_SCAN_FAILED;
        ctx->snapshot->scan_result = ESP_ERR_NO_MEM;
        ++ctx->snapshot->generation;
        snapshot_unlock(ctx);
        app_storage_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t factory_nand_update_get_snapshot(factory_nand_update_snapshot_t *snapshot)
{
    nand_update_context_t *ctx = &s_catalog;
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "NAND snapshot output is null");
    ESP_RETURN_ON_FALSE(ctx->mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "NAND catalog is not initialized");
    if (xSemaphoreTake(ctx->mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *snapshot = (*ctx->snapshot);
    (void)xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

esp_err_t factory_nand_update_init(void)
{
    nand_update_context_t *ctx = &s_catalog;
    if (ctx->mutex == NULL) {
        ctx->mutex = xSemaphoreCreateMutexStatic(&ctx->mutex_storage);
    }
    return ctx->mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

#else

esp_err_t factory_nand_install_idea(const char *path, const char *manifest,
                                    esp_err_t (*before_commit)(void *), void *ctx)
{
    (void)path;
    (void)manifest;
    (void)before_commit;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_start_nand(const char *manifest_path)
{
    (void)manifest_path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_nand_update_init(void)
{
    return ESP_OK;
}

esp_err_t factory_nand_update_request_scan(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_nand_update_get_snapshot(factory_nand_update_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
