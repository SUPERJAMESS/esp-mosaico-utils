// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

void ideas_hex(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}

esp_err_t ideas_verify_file(const ideas_file_t *file, const char *path, void *ctx)
{
    (void)ctx;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != file->size)
        return ESP_ERR_INVALID_SIZE;
    FILE *f = fopen(path, "rb");
    uint8_t *buffer = malloc(IO_BYTES);
    if (!f || !buffer) {
        if (f)
            fclose(f);
        free(buffer);
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err =
        psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS
            ? ESP_OK
            : ESP_FAIL;
    size_t n, total = 0;
    while (err == ESP_OK && (n = fread(buffer, 1, IO_BYTES, f))) {
        total += n;
        if (total > file->size || psa_hash_update(&hash, buffer, n) != PSA_SUCCESS)
            err = ESP_FAIL;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    size_t size = 0;
    char actual[65];
    if (err == ESP_OK &&
        (ferror(f) || total != file->size ||
         psa_hash_finish(&hash, digest, sizeof(digest), &size) != PSA_SUCCESS || size != 32))
        err = ESP_FAIL;
    if (err == ESP_OK) {
        ideas_hex(digest, actual);
        if (strcmp(actual, file->sha256))
            err = ESP_ERR_INVALID_CRC;
    }
    psa_hash_abort(&hash);
    fclose(f);
    free(buffer);
    return err;
}

esp_err_t ideas_flash_hash(uint32_t offset, uint32_t size, const char *expected)
{
    uint8_t *buffer = malloc(IO_BYTES);
    if (!buffer)
        return ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err =
        psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS
            ? ESP_OK
            : ESP_FAIL;
    for (uint32_t done = 0; err == ESP_OK && done < size;) {
        size_t n = size - done < IO_BYTES ? size - done : IO_BYTES;
        err = esp_flash_read(esp_flash_default_chip, buffer, offset + done, n);
        if (err == ESP_OK && psa_hash_update(&hash, buffer, n) != PSA_SUCCESS)
            err = ESP_FAIL;
        done += n;
        vTaskDelay(1);
    }
    uint8_t digest[32];
    size_t n;
    char actual[65];
    if (err == ESP_OK && (psa_hash_finish(&hash, digest, 32, &n) != PSA_SUCCESS || n != 32))
        err = ESP_FAIL;
    if (err == ESP_OK) {
        ideas_hex(digest, actual);
        if (strcmp(expected, actual))
            err = ESP_ERR_INVALID_CRC;
    }
    psa_hash_abort(&hash);
    free(buffer);
    return err;
}

bool ideas_boot_record(boot_record_t *record)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "ideas", NVS_READONLY, &nvs) !=
        ESP_OK)
        return false;
    size_t size = sizeof(*record);
    esp_err_t err = nvs_get_blob(nvs, "boot", record, &size);
    nvs_close(nvs);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (err != ESP_OK || size != sizeof(*record) || record->schema != 1 ||
        !memchr(record->code, 0, sizeof(record->code)) || !ideas_code_valid(record->code) ||
        !memchr(record->application_id, 0, sizeof(record->application_id)) || record->image[64] ||
        record->layout[64] || !boot || boot->address != record->offset ||
        record->offset != 0x210000 || !record->size || record->size > 16U * 1024U * 1024U)
        return false;
    return ideas_flash_hash(0x8000, 4096, record->layout) == ESP_OK &&
           ideas_flash_hash(record->offset, record->size, record->image) == ESP_OK;
}

esp_err_t ideas_save_boot(void *ctx)
{
    const ideas_package_t *p = ctx;
    boot_record_t record = {
        .schema = 1, .offset = p->application_offset, .size = p->application_size};
    strcpy(record.code, p->code);
    strcpy(record.application_id, p->application_id);
    strcpy(record.release_id, p->release_id);
    strcpy(record.image, p->application_sha256);
    strcpy(record.layout, p->layout_sha256);
    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "ideas", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(nvs, "boot", &record, sizeof(record));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t ideas_scan(ideas_context_t *ctx)
{
    DIR *dir = opendir(ROOT);
    if (!dir && errno != ENOENT)
        return ESP_FAIL;
    factory_idea_t *items = calloc(IDEAS_MAX_ITEMS, sizeof(*items));
    ideas_package_t *p = malloc(sizeof(*p));
    if (!items || !p) {
        if (dir)
            closedir(dir);
        free(items);
        free(p);
        return ESP_ERR_NO_MEM;
    }
    boot_record_t boot = {0};
    bool has_boot = ideas_boot_record(&boot);
    icon_t icons[IDEAS_MAX_ITEMS] = {0};
    struct dirent *entry;
    size_t count = 0, invalid = 0, examined = 0;
    while (dir && examined++ < 64 && (entry = readdir(dir))) {
        if (!ideas_code_valid(entry->d_name))
            continue;
        if (ideas_store_recover(ROOT, entry->d_name) != ESP_OK ||
            ideas_store_read(ROOT, entry->d_name, p) != ESP_OK) {
            invalid++;
            continue;
        }
        if (count == IDEAS_MAX_ITEMS) {
            invalid++;
            continue;
        }
        factory_idea_t *item = &items[count++];
        strcpy(item->code, p->code);
        strcpy(item->application_id, p->application_id);
        strcpy(item->release_id, p->release_id);
        strcpy(item->version, p->version);
        strcpy(item->name, p->name);
        strcpy(item->description, p->description);
        strcpy(item->presentation, p->presentation);
        strcpy(item->icon, p->icon);
        item->bytes = p->total_size;
        item->components = p->component_count;
        /* An updated NAND copy keeps the badge for the same application, even
         * while NOR still contains the older release. */
        item->boot = has_boot && !strcmp(boot.code, p->code) &&
                     !strcmp(boot.application_id, p->application_id);
        for (size_t i = 0; i < p->file_count; i++) {
            const ideas_file_t *file = &p->files[i];
            if (strcmp(file->name, p->icon) || file->size > 65536)
                continue;
            char path[IDEAS_PATH_BYTES];
            if (ideas_store_path(ROOT, p->code, p->icon, path) != ESP_OK ||
                ideas_verify_file(file, path, NULL) != ESP_OK)
                break;
            FILE *f = fopen(path, "rb");
            unsigned char header[24];
            /* Bounded PNG dimensions prevent a tiny encoded icon from asking
             * the asynchronous image decoder for a huge surface. */
            bool valid = f && fread(header, 1, sizeof(header), f) == sizeof(header) &&
                         !memcmp(header, "\x89PNG\r\n\x1a\n", 8) &&
                         !memcmp(header + 12, "IHDR", 4) && !header[16] && !header[17] &&
                         !header[18] && header[19] && header[19] <= 96 && !header[20] &&
                         !header[21] && !header[22] && header[23] && header[23] <= 96;
            if (valid) {
                void *data = malloc(file->size);
                rewind(f);
                if (data && fread(data, 1, file->size, f) == file->size && !ferror(f))
                    icons[count - 1] = (icon_t){data, file->size};
                else
                    free(data);
            }
            if (f)
                fclose(f);
            break;
        }
    }
    if (dir)
        closedir(dir);
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    for (size_t i = 0; i < IDEAS_MAX_ITEMS; i++) {
        free(ctx->icons[i].data);
        ctx->icons[i] = icons[i];
    }
    memcpy(ctx->state->items, items, IDEAS_MAX_ITEMS * sizeof(*items));
    ctx->state->count = count;
    ctx->state->invalid = invalid;
    ctx->state->scan_error = ESP_OK;
    ++ctx->state->generation;
    xSemaphoreGive(ctx->lock);
    free(items);
    free(p);
    return ESP_OK;
}

esp_err_t ideas_run_local(ideas_context_t *ctx, job_t *job)
{
    if (job->command == IDEAS_SCAN)
        return ideas_scan(ctx);
    if (job->command == IDEAS_DOWNLOAD) {
        pending_t *p = job->pending;
        ideas_message(ctx, "Downloading Idea", "Saving and verifying files in NAND");
        esp_err_t err = ideas_store_download(ROOT, p->json, p->json_size, &p->package,
                                             ideas_fetch_file, ideas_verify_file, p);
        if (err == ESP_OK) {
            (void)ideas_scan(ctx);
            ideas_message(ctx, "Download complete",
                          "Saved in My Ideas.\nSelect it to set as boot app.");
        }
        return err;
    }
    ideas_package_t *local = calloc(1, sizeof(*local));
    if (!local)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ideas_store_recover(ROOT, job->code);
    if (err == ESP_OK)
        err = ideas_store_read(ROOT, job->code, local);
    if (job->command == IDEAS_PREVIEW) {
        job->pending = calloc(1, sizeof(*job->pending));
        if (!job->pending)
            err = ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            job->pending->package = *local;
            job->pending->view_only = true;
            err = ideas_prepare_assets(ctx, job, job->pending, true);
        }
    } else if (job->command == IDEAS_INSTALL) {
        ideas_message(ctx, "Setting boot application", "Verifying downloaded files");
        if (err == ESP_OK)
            err = ideas_store_verify(ROOT, local, ideas_verify_file, NULL);
        char path[IDEAS_PATH_BYTES];
        if (err == ESP_OK)
            err = ideas_store_path(ROOT, job->code, "manifest.json", path);
        if (err == ESP_OK)
            err = factory_nand_install_idea(path, local->manifest, ideas_save_boot, local);
        if (err == ESP_OK)
            ideas_message(ctx, "Boot application ready",
                          "Starts on next power-on.\nRestart to run it now.");
        (void)ideas_scan(ctx);
    }
    free(local);
    return err;
}
