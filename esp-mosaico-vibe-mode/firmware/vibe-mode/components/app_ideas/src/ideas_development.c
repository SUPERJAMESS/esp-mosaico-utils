// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"
#include "factory_system_update.h"
#include "esp_iris.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"
#include "mosaico_boot.h"

typedef struct {
    ideas_package_t package;
    uint32_t offsets[8];
    char *firmware;
    esp_app_desc_t app;
    esp_partition_info_t entries[ESP_PARTITION_TABLE_MAX_ENTRIES];
    ideas_context_t *owner;
} development_t;

static bool ota_idle(void)
{
    esp_iris_ota_status_t status = {0};
    esp_err_t err = esp_iris_ota_get_status(&status);
    return (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) && !status.active;
}
static esp_err_t flash_digest(ideas_context_t *ctx, uint32_t offset, uint32_t size, char result[65])
{
    unsigned char *buffer = malloc(IO_BYTES);
    if (!buffer) return ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (uint32_t pos = 0; err == ESP_OK && pos < size;) {
        if (ctx && ideas_job_cancelled(ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        size_t n = size - pos < IO_BYTES ? size - pos : IO_BYTES;
        err = esp_flash_read(esp_flash_default_chip, buffer, offset + pos, n);
        if (err == ESP_OK && psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) err = ESP_FAIL;
        pos += n;
        vTaskDelay(1);
    }
    unsigned char digest[32]; size_t n = 0;
    if (err == ESP_OK && (psa_hash_finish(&hash, digest, sizeof(digest), &n) != PSA_SUCCESS || n != 32)) err = ESP_FAIL;
    if (err == ESP_OK) ideas_hex(digest, result);
    psa_hash_abort(&hash); free(buffer);
    return err;
}
static esp_err_t add_file(development_t *dev, uint32_t offset, uint32_t size)
{
    ideas_package_t *p = &dev->package;
    if (p->file_count == 8 || p->total_size + size > IDEAS_MAX_PACKAGE_BYTES) return ESP_ERR_INVALID_SIZE;
    size_t i = p->file_count;
    ideas_file_t *file = &p->files[i];
    snprintf(file->name, sizeof(file->name), "firmware/part-%u.bin", (unsigned)i);
    snprintf(file->path, sizeof(file->path), "/local/part-%u.bin", (unsigned)i);
    file->size = size; dev->offsets[i] = offset;
    esp_err_t err = flash_digest(dev->owner, offset, size, file->sha256);
    if (err == ESP_OK) { ++p->file_count; ++p->component_count; p->total_size += size; }
    return err;
}
static esp_err_t inspect(development_t *dev)
{
    if (!ota_idle() || (dev->owner && ideas_job_cancelled(dev->owner))) return ESP_ERR_INVALID_STATE;
    /* Iris may have committed a different layout since boot. Read the live
     * table rather than ESP-IDF's cached esp_partition list. */
    uint8_t *table = malloc(4096);
    if (!table) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_flash_read(esp_flash_default_chip, table, 0x8000, 4096);
    int count = 0;
    if (err == ESP_OK) err = esp_partition_table_verify((const esp_partition_info_t *)table, false, &count);
    esp_partition_info_t *entries = dev->entries;
    if (err == ESP_OK && count > 0 && count <= ESP_PARTITION_TABLE_MAX_ENTRIES)
        memcpy(entries, table, count * sizeof(entries[0]));
    else if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
    free(table);
    if (err != ESP_OK) return err;
    const esp_partition_info_t *app = NULL;
    for (int i = 0; i < count; ++i)
        if (entries[i].type == ESP_PARTITION_TYPE_APP && entries[i].subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            if (app) return ESP_ERR_INVALID_ARG;
            app = &entries[i];
        }
    if (!app || app->pos.offset != 0x210000) return ESP_ERR_NOT_FOUND;
    esp_image_metadata_t metadata = {0};
    err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &app->pos, &metadata);
    if (err == ESP_OK) err = esp_flash_read(esp_flash_default_chip, &dev->app,
        app->pos.offset + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(dev->app));
    if (err != ESP_OK) return err;
    ideas_package_t *p = &dev->package;
    if (!memchr(dev->app.project_name, 0, sizeof(dev->app.project_name)) ||
        !memchr(dev->app.version, 0, sizeof(dev->app.version))) return ESP_ERR_INVALID_ARG;
    snprintf(p->name, sizeof(p->name), "%s", dev->app.project_name[0] ? dev->app.project_name : "Development Idea");
    snprintf(p->version, sizeof(p->version), "%s", dev->app.version[0] ? dev->app.version : "development");
    snprintf(p->description, sizeof(p->description), "Saved from USB development. Available offline.");
    strcpy(p->locale, "en");
    p->application_offset = app->pos.offset; p->application_size = metadata.image_len;
    err = esp_flash_get_size(esp_flash_default_chip, &p->flash_size);
    if (err == ESP_OK) err = add_file(dev, 0x8000, 4096);
    if (err == ESP_OK) err = add_file(dev, app->pos.offset, metadata.image_len);
    if (err != ESP_OK) return err;
    strcpy(p->layout_sha256, p->files[0].sha256);
    strcpy(p->application_sha256, p->files[1].sha256);
    /* Preserve the application's resource partitions, never sysmeta, app NVS,
     * OTA selectors, bootloader, Recovery or crash evidence. */
    for (int i = 0; i < count && err == ESP_OK; ++i) {
        const esp_partition_info_t *part = &entries[i];
        if (part->type == ESP_PARTITION_TYPE_DATA && part->subtype >= 0x06 && part->subtype != 0x90 &&
            part->pos.offset >= app->pos.offset + app->pos.size)
            err = add_file(dev, part->pos.offset, part->pos.size);
    }
    if (err != ESP_OK) return err;
    unsigned char digest[32]; size_t n = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_FAIL;
    for (size_t i = 0; i < p->file_count; ++i)
        if (psa_hash_update(&hash, (const unsigned char *)p->files[i].sha256, 64) != PSA_SUCCESS) err = ESP_FAIL;
    if (psa_hash_finish(&hash, digest, sizeof(digest), &n) != PSA_SUCCESS || n != 32) err = ESP_FAIL;
    psa_hash_abort(&hash);
    if (err != ESP_OK || !ota_idle()) return ESP_ERR_INVALID_STATE;
    ideas_hex(digest, p->release_id);
    snprintf(p->application_id, sizeof(p->application_id), "development-%.32s", dev->app.project_name);
    return ESP_OK;
}
static cJSON *descriptor(development_t *dev)
{
    ideas_package_t *p = &dev->package;
    cJSON *manifest = cJSON_CreateObject();
    if (!manifest) return NULL;
    cJSON_AddStringToObject(manifest, "schema", "esp-iris-system-update/0.2");
    cJSON_AddStringToObject(manifest, "release", p->version);
    cJSON_AddStringToObject(manifest, "target_layout_sha256", p->layout_sha256);
    cJSON *target = cJSON_AddObjectToObject(manifest, "target");
    cJSON_AddNumberToObject(target, "chip_id", 32);
    cJSON_AddNumberToObject(target, "flash_size", p->flash_size);
    cJSON *components = cJSON_AddArrayToObject(manifest, "components");
    for (size_t i = 0; i < p->file_count; ++i) {
        cJSON *component = cJSON_CreateObject();
        cJSON_AddNumberToObject(component, "id", i + 1);
        cJSON_AddStringToObject(component, "kind", i == 0 ? "partition_table" : i == 1 ? "application" : "data");
        cJSON_AddStringToObject(component, "file", strrchr(p->files[i].name, '/') + 1);
        cJSON_AddNumberToObject(component, "target_offset", dev->offsets[i]);
        cJSON_AddNumberToObject(component, "size", p->files[i].size);
        cJSON_AddStringToObject(component, "sha256", p->files[i].sha256);
        cJSON_AddItemToArray(components, component);
    }
    dev->firmware = cJSON_PrintUnformatted(manifest);
    cJSON_Delete(manifest);
    char sha[65];
    if (!dev->firmware || !ideas_hash_data(dev->firmware, strlen(dev->firmware), sha)) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "schema", "mosaico-ideas/local/2");
    cJSON_AddStringToObject(root, "release_id", p->release_id);
    cJSON *app = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(app, "code", p->code);
    cJSON_AddStringToObject(app, "id", p->application_id);
    cJSON_AddStringToObject(app, "name", p->name);
    cJSON_AddStringToObject(app, "description", p->description);
    cJSON *firmware = cJSON_AddObjectToObject(root, "firmware");
    cJSON *ref = cJSON_AddObjectToObject(firmware, "manifest");
    cJSON_AddStringToObject(ref, "path", "firmware/manifest.json");
    cJSON_AddNumberToObject(ref, "size", strlen(dev->firmware));
    cJSON_AddStringToObject(ref, "sha256", sha);
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "profile", "esp-mosaico/v2");
    cJSON_AddStringToObject(device, "default_locale", "en");
    cJSON_AddArrayToObject(device, "presentations");
    return root;
}
static esp_err_t copy_flash(const ideas_file_t *file, const char *path, void *argument)
{
    development_t *dev = argument;
    if (ideas_job_cancelled(dev->owner)) return ESP_ERR_INVALID_STATE;
    if (!strcmp(file->name, "firmware/manifest.json")) {
        FILE *out = fopen(path, "wb");
        if (!out) return ESP_FAIL;
        bool ok = fwrite(dev->firmware, 1, file->size, out) == file->size;
        if (fflush(out) || fsync(fileno(out))) ok = false;
        if (fclose(out)) ok = false;
        return ok ? ESP_OK : ESP_FAIL;
    }
    size_t i = 0;
    while (i < dev->package.file_count && strcmp(file->name, dev->package.files[i].name)) ++i;
    if (i == dev->package.file_count) return ESP_ERR_INVALID_ARG;
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    unsigned char *buffer = malloc(IO_BYTES);
    int64_t started = factory_io_now();
    FILE *out = fopen(path, "wb");
    metrics.begin = factory_io_now() - started;
    esp_err_t err = buffer && out ? ESP_OK : ESP_ERR_NO_MEM;
    for (uint32_t pos = 0; err == ESP_OK && pos < file->size;) {
        if (ideas_job_cancelled(dev->owner)) { err = ESP_ERR_INVALID_STATE; break; }
        size_t n = file->size - pos < IO_BYTES ? file->size - pos : IO_BYTES;
        started = factory_io_now();
        err = esp_flash_read(esp_flash_default_chip, buffer, dev->offsets[i] + pos, n);
        metrics.read += factory_io_now() - started;
        if (err == ESP_OK) {
            started = factory_io_now();
            if (fwrite(buffer, 1, n, out) != n) err = ESP_FAIL;
            metrics.write += factory_io_now() - started;
        }
        if (err == ESP_OK) {
            metrics.bytes += n;
            xSemaphoreTake(dev->owner->lock, portMAX_DELAY);
            dev->owner->state->received += n;
            xSemaphoreGive(dev->owner->lock);
        }
        pos += n; vTaskDelay(1);
    }
    if (out) {
        started = factory_io_now();
        if (fflush(out) || fsync(fileno(out))) err = ESP_FAIL;
        if (fclose(out)) err = ESP_FAIL;
        metrics.sync = factory_io_now() - started;
    }
    factory_io_report("development_copy", file->name, &metrics, buffer, IO_BYTES, err);
    free(buffer);
    return err;
}
static esp_err_t capture_cancel(uint32_t checked, void *argument)
{
    (void)checked;
    development_t *dev = argument;
    return ideas_job_cancelled(dev->owner) ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static esp_err_t verify_capture(const ideas_file_t *file, const char *path, void *argument)
{
    return ideas_hash_file_progress(path, file->size, file->sha256, capture_cancel, argument);
}
static esp_err_t capture_gate(void *argument)
{
    development_t *dev = argument;
    if (!ota_idle()) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < dev->package.file_count; ++i) {
        char digest[65];
        esp_err_t err = flash_digest(dev->owner, dev->offsets[i], dev->package.files[i].size, digest);
        if (err != ESP_OK) return err;
        if (strcmp(digest, dev->package.files[i].sha256)) return ESP_ERR_INVALID_CRC;
    }
    if (!ota_idle()) return ESP_ERR_INVALID_STATE;
    /* Atomically close cancellation before publishing the verified generation.
     * A request accepted earlier must roll back .incoming, never report a
     * cancelled result after making the saved copy visible. */
    xSemaphoreTake(dev->owner->lock, portMAX_DELAY);
    esp_err_t err = dev->owner->cancel_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) dev->owner->state->cancellable = false;
    xSemaphoreGive(dev->owner->lock);
    return err;
}
void ideas_development_status(ideas_context_t *ctx)
{
    esp_err_t err = factory_system_update_read_acquire();
    if (err != ESP_OK) return;
    esp_app_desc_t app = {0};
    /* Discovery reads only the fixed-layout app description; user actions verify the image. */
    err = esp_flash_read(esp_flash_default_chip, &app,
        0x210000 + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(app));
    bool available = err == ESP_OK && app.magic_word == ESP_APP_DESC_MAGIC_WORD &&
        memchr(app.project_name, 0, sizeof(app.project_name)) && memchr(app.version, 0, sizeof(app.version));
    factory_system_update_read_release();
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->state->development_available = available;
    /* Without a full audit, saved identity is unknown until an explicit save succeeds. */
    ctx->state->development_saved = false;
    ctx->development_release_id[0] = 0;
    snprintf(ctx->state->development_name, sizeof(ctx->state->development_name), "%s", available ? app.project_name : "");
    snprintf(ctx->state->development_version, sizeof(ctx->state->development_version), "%s", available ? app.version : "");
    xSemaphoreGive(ctx->lock);
}
esp_err_t ideas_save_development(ideas_context_t *ctx)
{
    esp_err_t err = ideas_load_library(ctx);
    if (err != ESP_OK) return err;
    err = factory_system_update_read_acquire();
    if (err != ESP_OK) return err;
    development_t *dev = calloc(1, sizeof(*dev));
    if (!dev) { factory_system_update_read_release(); return ESP_ERR_NO_MEM; }
    dev->owner = ctx;
    ideas_message(ctx, "Saving development Idea", "Reading application and resources");
    err = inspect(dev);
    ideas_package_t *p = &dev->package;
    bool found = false;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    for (size_t i = 0; err == ESP_OK && i < ctx->state->count; ++i)
        if (!strcmp(ctx->state->items[i].release_id, p->release_id)) { strcpy(p->code, ctx->state->items[i].code); found = true; }
    if (err == ESP_OK && !found && ctx->state->count >= IDEAS_MAX_ITEMS) err = ESP_ERR_NO_MEM;
    ctx->state->total = p->total_size; ctx->state->received = 0;
    xSemaphoreGive(ctx->lock);
    /* Resolve short-name collisions without ever replacing an unrelated copy. */
    ideas_package_t *existing = calloc(1, sizeof(*existing));
    if (!existing) err = ESP_ERR_NO_MEM;
    for (unsigned salt = 0; err == ESP_OK && !found && salt < 60; ++salt) {
        snprintf(p->code, sizeof(p->code), "d%.5s", p->release_id + salt);
        esp_err_t read = ideas_store_read(ROOT, p->code, existing);
        if (read == ESP_ERR_NOT_FOUND || (read == ESP_OK && !strcmp(existing->release_id, p->release_id))) found = true;
    }
    if (err == ESP_OK && !found) err = ESP_ERR_INVALID_STATE;
    cJSON *root = err == ESP_OK ? descriptor(dev) : NULL;
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    if (err == ESP_OK && !json) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = ideas_package_parse(json, strlen(json), p->code, existing);
    if (err == ESP_OK) err = ideas_package_firmware(existing, dev->firmware, strlen(dev->firmware));
    if (err == ESP_OK) {
        ideas_message(ctx, "Saving development Idea", "Scanning NAND...");
        err = ideas_check_space(existing->total_size);
    }
    if (err == ESP_OK) {
        ideas_message(ctx, "Saving development Idea", "Transferring files");
        err = ideas_store_download_gated(ROOT, json, strlen(json), existing, copy_flash,
                                        verify_capture, dev, NULL, capture_gate);
    }
    if (err == ESP_OK) err = ideas_store_finish(ROOT, existing);
    if (err == ESP_OK) err = ideas_save_boot(existing);
    if (err == ESP_OK) {
        ctx->boot_verified = ideas_boot_record(&ctx->verified_boot, false);
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        ctx->state->development_available = true;
        snprintf(ctx->development_release_id, sizeof(ctx->development_release_id), "%s", p->release_id);
        xSemaphoreGive(ctx->lock);
    }
    cJSON_free(json); cJSON_Delete(root); cJSON_free(dev->firmware); free(existing); free(dev);
    factory_system_update_read_release();
    if (err == ESP_OK) { (void)ideas_scan(ctx, false); ideas_message(ctx, "Development Idea saved", "Available in My Ideas"); }
    return err;
}

esp_err_t ideas_run_development(void)
{
    esp_err_t err = factory_system_update_read_acquire();
    if (err != ESP_OK) return err;
    development_t *dev = calloc(1, sizeof(*dev));
    err = dev ? inspect(dev) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = mosaico_boot_select(dev->package.application_offset);
    free(dev);
    if (err == ESP_OK) esp_restart();
    factory_system_update_read_release();
    return err;
}
