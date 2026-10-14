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
static esp_err_t flash_digest(uint32_t offset, uint32_t size, char result[65])
{
    unsigned char *buffer = malloc(IO_BYTES);
    if (!buffer) return ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (uint32_t pos = 0; err == ESP_OK && pos < size;) {
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
    snprintf(file->name, sizeof(file->name), "part-%u.bin", (unsigned)i);
    snprintf(file->path, sizeof(file->path), "/local/part-%u.bin", (unsigned)i);
    file->size = size; dev->offsets[i] = offset;
    esp_err_t err = flash_digest(offset, size, file->sha256);
    if (err == ESP_OK) { ++p->file_count; ++p->component_count; p->total_size += size; }
    return err;
}
static esp_err_t inspect(development_t *dev)
{
    if (!ota_idle()) return ESP_ERR_INVALID_STATE;
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
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "schema", "mosaico-ideas/local/1");
    cJSON *app = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(app, "code", p->code);
    cJSON_AddStringToObject(app, "id", p->application_id);
    cJSON_AddStringToObject(app, "release_id", p->release_id);
    cJSON_AddStringToObject(app, "name", p->name);
    cJSON_AddStringToObject(app, "version", p->version);
    cJSON_AddStringToObject(app, "description", p->description);
    cJSON_AddStringToObject(app, "locale", p->locale);
    cJSON *manifest = cJSON_AddObjectToObject(root, "system_manifest");
    cJSON_AddStringToObject(manifest, "schema", "esp-iris-system-update/0.2");
    cJSON_AddStringToObject(manifest, "release", p->version);
    cJSON_AddStringToObject(manifest, "target_layout_sha256", p->layout_sha256);
    cJSON *target = cJSON_AddObjectToObject(manifest, "target");
    cJSON_AddNumberToObject(target, "chip_id", 32);
    cJSON_AddNumberToObject(target, "flash_size", p->flash_size);
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    cJSON *components = cJSON_AddArrayToObject(manifest, "components");
    for (size_t i = 0; i < p->file_count; ++i) {
        cJSON *file = cJSON_CreateObject(), *component = cJSON_CreateObject();
        cJSON_AddStringToObject(file, "name", p->files[i].name);
        cJSON_AddStringToObject(file, "path", p->files[i].path);
        cJSON_AddNumberToObject(file, "size", p->files[i].size);
        cJSON_AddStringToObject(file, "sha256", p->files[i].sha256);
        cJSON_AddItemToArray(files, file);
        cJSON_AddNumberToObject(component, "id", i + 1);
        cJSON_AddStringToObject(component, "kind", i == 0 ? "partition_table" : i == 1 ? "application" : "data");
        cJSON_AddStringToObject(component, "file", p->files[i].name);
        cJSON_AddNumberToObject(component, "target_offset", dev->offsets[i]);
        cJSON_AddNumberToObject(component, "size", p->files[i].size);
        cJSON_AddStringToObject(component, "sha256", p->files[i].sha256);
        cJSON_AddItemToArray(components, component);
    }
    return root;
}
static esp_err_t copy_flash(const ideas_file_t *file, const char *path, void *argument)
{
    development_t *dev = argument;
    size_t i = 0;
    while (i < dev->package.file_count && strcmp(file->name, dev->package.files[i].name)) ++i;
    if (i == dev->package.file_count) return ESP_ERR_INVALID_ARG;
    unsigned char *buffer = malloc(IO_BYTES);
    FILE *out = fopen(path, "wb");
    esp_err_t err = buffer && out ? ESP_OK : ESP_ERR_NO_MEM;
    for (uint32_t pos = 0; err == ESP_OK && pos < file->size;) {
        size_t n = file->size - pos < IO_BYTES ? file->size - pos : IO_BYTES;
        err = esp_flash_read(esp_flash_default_chip, buffer, dev->offsets[i] + pos, n);
        if (err == ESP_OK && fwrite(buffer, 1, n, out) != n) err = ESP_FAIL;
        if (err == ESP_OK) {
            xSemaphoreTake(dev->owner->lock, portMAX_DELAY);
            dev->owner->state->received += n;
            xSemaphoreGive(dev->owner->lock);
        }
        pos += n; vTaskDelay(1);
    }
    if (out) {
        if (fflush(out) || fsync(fileno(out))) err = ESP_FAIL;
        if (fclose(out)) err = ESP_FAIL;
    }
    free(buffer);
    return err;
}
static esp_err_t capture_gate(void *argument)
{
    development_t *dev = argument;
    if (!ota_idle()) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < dev->package.file_count; ++i) {
        esp_err_t err = ideas_flash_hash(dev->offsets[i], dev->package.files[i].size, dev->package.files[i].sha256);
        if (err != ESP_OK) return err;
    }
    return ota_idle() ? ESP_OK : ESP_ERR_INVALID_STATE;
}
void ideas_development_status(ideas_context_t *ctx)
{
    esp_err_t lease = factory_system_update_read_acquire();
    if (lease != ESP_OK) {
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        ctx->state->development_available = false;
        ctx->state->development_saved = false;
        xSemaphoreGive(ctx->lock);
        return;
    }
    development_t *dev = calloc(1, sizeof(*dev));
    esp_err_t err = dev ? inspect(dev) : ESP_ERR_NO_MEM;
    bool saved = false;
    char boot_code[7] = "";
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (err == ESP_OK)
        for (size_t i = 0; i < ctx->state->count; ++i) {
            saved |= !strcmp(ctx->state->items[i].release_id, dev->package.release_id);
            if (ctx->state->items[i].boot) strcpy(boot_code, ctx->state->items[i].code);
        }
    xSemaphoreGive(ctx->lock);
    if (err == ESP_OK && !saved && boot_code[0]) {
        ideas_package_t *package = malloc(sizeof(*package));
        saved = package && ideas_store_read(ROOT, boot_code, package) == ESP_OK &&
                ideas_verify_installed(package) == ESP_OK;
        free(package);
    }
    factory_system_update_read_release();
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->state->development_available = err == ESP_OK;
    ctx->state->development_saved = saved;
    if (err == ESP_OK) {
        snprintf(ctx->state->development_name, sizeof(ctx->state->development_name), "%s", dev->app.project_name);
        snprintf(ctx->state->development_version, sizeof(ctx->state->development_version), "%s", dev->app.version);
    }
    xSemaphoreGive(ctx->lock);
    free(dev);
}
esp_err_t ideas_save_development(ideas_context_t *ctx)
{
    esp_err_t err = factory_system_update_read_acquire();
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
    if (err == ESP_OK) err = ideas_check_space(p->total_size);
    if (err == ESP_OK) {
        strcpy(p->manifest, existing->manifest);
        err = ideas_store_download_gated(ROOT, json, strlen(json), p, copy_flash,
                                         ideas_verify_file, dev, capture_gate);
    }
    if (err == ESP_OK) err = ideas_save_boot(p);
    cJSON_free(json); cJSON_Delete(root); free(existing); free(dev);
    factory_system_update_read_release();
    if (err == ESP_OK) { (void)ideas_scan(ctx); ideas_message(ctx, "Development Idea saved", "Available in My Ideas"); }
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
