// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

void ideas_hex(const uint8_t digest[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}

esp_err_t ideas_verify_file(const ideas_file_t *file, const char *path, void *ctx) {
    (void)ctx;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != file->size)
        return ESP_ERR_INVALID_SIZE;
    return ideas_hash_file(path, file->size, file->sha256) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

typedef struct { ideas_context_t *ctx; uint64_t base; } verify_progress_t;
static void checked_bytes(uint32_t checked, void *arg) {
    verify_progress_t *p = arg;
    xSemaphoreTake(p->ctx->lock, portMAX_DELAY);
    p->ctx->state->received = p->base + checked;
    xSemaphoreGive(p->ctx->lock);
}
static esp_err_t verify_download_file(const ideas_file_t *file, const char *path, void *arg) {
    ideas_context_t *ctx = ((pending_t *)arg)->owner;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != file->size)
        return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    verify_progress_t p = {.ctx = ctx, .base = ctx->state->received};
    xSemaphoreGive(ctx->lock);
    bool ok = ideas_hash_file_progress(path, file->size, file->sha256, checked_bytes, &p);
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (ok) ++ctx->state->files_verified;
    xSemaphoreGive(ctx->lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_CRC;
}
static void download_stage(ideas_store_stage_t stage, void *arg) {
    ideas_context_t *ctx = ((pending_t *)arg)->owner;
    const char *title;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (stage == IDEAS_STORE_VERIFY) {
        ctx->state->phase = IDEAS_PHASE_VERIFY_FILES;
        ctx->state->received = 0;
        title = "Verifying files";
    } else if (stage == IDEAS_STORE_IMAGE) {
        ctx->state->phase = IDEAS_PHASE_CHECK_IMAGE;
        title = "Checking image";
    } else {
        ctx->state->phase = IDEAS_PHASE_SAVE;
        title = "Saving Idea";
    }
    snprintf(ctx->state->title, sizeof(ctx->state->title), "%s", title);
    snprintf(ctx->state->detail, sizeof(ctx->state->detail), "Please wait");
    xSemaphoreGive(ctx->lock);
#ifdef ESP_PLATFORM
    ESP_LOGI("ideas_stage", "stage=%u", (unsigned)stage);
#endif
}

esp_err_t ideas_flash_hash(uint32_t offset, uint32_t size, const char *expected) {
    factory_io_metrics_t metrics = {.started = factory_io_now()};
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
        int64_t started = factory_io_now();
        err = esp_flash_read(esp_flash_default_chip, buffer, offset + done, n);
        metrics.read += factory_io_now() - started;
        started = factory_io_now();
        if (err == ESP_OK && psa_hash_update(&hash, buffer, n) != PSA_SUCCESS)
            err = ESP_FAIL;
        metrics.hash += factory_io_now() - started;
        metrics.bytes += n;
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
    factory_io_report("boot_nor_hash", "configured", &metrics, buffer, IO_BYTES, err);
    free(buffer);
    return err;
}

bool ideas_boot_record(boot_record_t *record, bool verify) {
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
    return !verify || (ideas_flash_hash(0x8000, 4096, record->layout) == ESP_OK &&
           ideas_flash_hash(record->offset, record->size, record->image) == ESP_OK);
}

esp_err_t ideas_save_boot(void *ctx) {
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

esp_err_t ideas_scan(ideas_context_t *ctx, bool audit_boot) {
    factory_io_metrics_t metrics = {.started = factory_io_now()};
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
    bool has_boot = ideas_boot_record(&boot, audit_boot);
    if (audit_boot) {
        ctx->boot_verified = has_boot;
        ctx->verified_boot = boot;
    } else {
        /* Reuse only NOR verification from this boot/our completed install.
         * A changed boot record or boot selection invalidates this result. */
        has_boot = has_boot && ctx->boot_verified &&
                   !memcmp(&boot, &ctx->verified_boot, sizeof(boot));
    }
    icon_t icons[IDEAS_MAX_ITEMS] = {0};
    struct dirent *entry;
    size_t count = 0, invalid = 0, examined = 0;
    while (dir && examined++ < 64 && (entry = readdir(dir))) {
        if (!ideas_code_valid(entry->d_name))
            continue;
        if (ideas_store_refresh(ROOT, entry->d_name, p) != ESP_OK ||
            ideas_store_select(ROOT, p, factory_locale_get()) != ESP_OK) {
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
    for (unsigned i = 0; i < 8; ++i) {
        free(ctx->titles[i].image.data);
        memset(&ctx->titles[i], 0, sizeof(ctx->titles[i]));
    }
    memcpy(ctx->state->items, items, IDEAS_MAX_ITEMS * sizeof(*items));
    ctx->state->count = count;
    ctx->state->invalid = invalid;
    ctx->state->scan_error = ESP_OK;
    ++ctx->state->generation;
    xSemaphoreGive(ctx->lock);
    free(items);
    free(p);
    factory_io_report("scan", audit_boot ? "audit" : "metadata", &metrics, NULL, 0, ESP_OK);
    return ESP_OK;
}

esp_err_t ideas_run_local(ideas_context_t *ctx, job_t *job) {
    if (job->command == IDEAS_ROW)
        return ideas_prepare_row(ctx, job);
    if (job->command == IDEAS_SCAN || job->command == IDEAS_RELOCALIZE)
        return ideas_scan(ctx, job->command == IDEAS_SCAN);
    if (job->command == IDEAS_DOWNLOAD) {
        pending_t *p = job->pending;
        ideas_message(ctx, "Downloading Idea", "Transferring files");
        esp_err_t err = ideas_store_download(ROOT, p->json, p->json_size, &p->package,
                                             ideas_fetch_file, verify_download_file, p, download_stage);
        if (err == ESP_OK)
            err = ideas_store_finish(ROOT, &p->package);
        if (err == ESP_OK) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            ctx->state->received = ctx->state->total;
            xSemaphoreGive(ctx->lock);
            (void)ideas_scan(ctx, false);
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            ctx->state->phase = IDEAS_PHASE_COMPLETE;
            xSemaphoreGive(ctx->lock);
            ideas_message(ctx, "Idea saved",
                          "Saved in My Ideas.\nSelect it to set as boot app.");
        }
        return err;
    }
    ideas_package_t *local = calloc(1, sizeof(*local));
    if (!local)
        return ESP_ERR_NO_MEM;
    if (job->command == IDEAS_INSTALL)
        ideas_message(ctx, "Setting boot application", "Verifying downloaded files");
    esp_err_t err = job->command == IDEAS_INSTALL
                        ? ideas_store_prepare(ROOT, job->code, local)
                        : ideas_store_refresh(ROOT, job->code, local);
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
        char path[IDEAS_PATH_BYTES];
        if (err == ESP_OK)
            err = ideas_store_path(ROOT, job->code, "firmware/manifest.json", path);
        if (err == ESP_OK) {
            ctx->boot_verified = false;
            err = factory_nand_install_idea(path, local->manifest, ideas_save_boot, local);
            if (err == ESP_OK)
                ctx->boot_verified = ideas_boot_record(&ctx->verified_boot, false);
        }
        if (err == ESP_OK)
            ideas_message(ctx, "Boot application ready",
                          "Starts on next power-on.\nRestart to run it now.");
        (void)ideas_scan(ctx, false);
    }
    free(local);
    return err;
}

esp_err_t ideas_prepare_row(ideas_context_t *ctx, job_t *job) {
    pending_t *pending = calloc(1, sizeof(*pending));
    if (!pending)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ideas_store_metadata(ROOT, job->code, &pending->package);
    if (err == ESP_OK)
        err = ideas_prepare_assets(ctx, job, pending, true);
    size_t size = 0;
    void *image = err == ESP_OK ? ideas_title_qoi(pending->pixels[0].data, &size) : NULL;
    if (err == ESP_OK && !image)
        err = ESP_ERR_NO_MEM;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    unsigned oldest = 0;
    for (unsigned i = 1; i < 8; ++i)
        if (ctx->titles[i].stamp < ctx->titles[oldest].stamp)
            oldest = i;
    free(ctx->titles[oldest].image.data);
    strcpy(ctx->titles[oldest].code, job->code);
    ctx->titles[oldest].language = factory_locale_get();
    ctx->titles[oldest].image = (icon_t){image, size};
    ctx->titles[oldest].stamp = ++ctx->title_clock;
    /* Failed entries also cache a miss until the next scan to avoid retry loops.
     */
    ++ctx->state->generation;
    xSemaphoreGive(ctx->lock);
    ideas_free_pending(pending);
    return err;
}
