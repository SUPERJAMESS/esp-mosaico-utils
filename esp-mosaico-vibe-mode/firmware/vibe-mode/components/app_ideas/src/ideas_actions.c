// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

esp_err_t ideas_check_space(uint64_t bytes)
{
    uint64_t total = 0, used = 0;
    esp_err_t err = app_storage_info(&total, &used);
    if (err != ESP_OK) return err;
    /* Keep the old generation until commit and leave room for LittleFS
     * metadata plus both descriptors. Never count its bytes as free space. */
    return used <= total && bytes <= total - used && total - used - bytes >= 65536
               ? ESP_OK : FACTORY_IDEAS_ERR_STORAGE_FULL;
}

bool ideas_job_cancelled(ideas_context_t *ctx)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    bool cancelled = ctx->cancel_requested;
    xSemaphoreGive(ctx->lock);
    return cancelled;
}
esp_err_t ideas_commit_download(void *argument)
{
    ideas_context_t *ctx = ((pending_t *)argument)->owner;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    esp_err_t err = ctx->cancel_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) ctx->state->cancellable = false;
    xSemaphoreGive(ctx->lock);
    return err;
}
esp_err_t ideas_delete(ideas_context_t *ctx, const char *code)
{
    boot_record_t boot;
    if (ideas_boot_record(&boot) && !strcmp(boot.code, code))
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = ideas_store_delete(ROOT, code);
    (void)ideas_scan(ctx);
    if (err == ESP_OK) ideas_message(ctx, "Idea deleted", "Removed from this device");
    return err;
}
esp_err_t ideas_update_all(ideas_context_t *ctx, job_t *job)
{
    char codes[IDEAS_MAX_ITEMS][7];
    size_t count = 0;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    for (size_t i = 0; i < ctx->state->count; ++i)
        if (ideas_code_valid(ctx->state->items[i].code)) {
            strcpy(codes[count++], ctx->state->items[i].code);
            ctx->state->items[i].queued = job->command == IDEAS_UPDATE_ALL;
        }
    ctx->state->batch_total = count;
    ctx->state->batch_index = 0;
    xSemaphoreGive(ctx->lock);
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        if (ideas_job_cancelled(ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        job_t item = {.command = IDEAS_CHECK, .owner = ctx, .metadata_only = job->command == IDEAS_REFRESH_UPDATES};
        strcpy(item.code, codes[i]);
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        if (++ctx->request_id == 0) ++ctx->request_id;
        ctx->state->preview_id = item.request_id = ctx->request_id;
        ctx->state->cancellable = true;
        ctx->state->batch_index = i + 1;
        strcpy(ctx->state->batch_code, item.code);
        ctx->state->received = ctx->state->total = 0;
        ++ctx->state->generation;
        snprintf(ctx->state->title, sizeof(ctx->state->title), "Updating Ideas %u/%u", (unsigned)i + 1, (unsigned)count);
        xSemaphoreGive(ctx->lock);
        err = ideas_prepare_download(ctx, &item);
        if (err == ESP_OK) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            for (size_t n = 0; n < ctx->state->count; ++n) if (!strcmp(ctx->state->items[n].code, item.code)) {
                factory_idea_t *entry = &ctx->state->items[n];
                entry->update_available = strcmp(entry->release_id, item.pending->package.release_id) != 0;
                snprintf(entry->latest_version, sizeof(entry->latest_version), "%s", item.pending->package.version);
            }
            ++ctx->state->generation;
            xSemaphoreGive(ctx->lock);
        }
        if (err == ESP_OK && !item.up_to_date && !item.metadata_only) {
            err = app_storage_acquire();
            if (err == ESP_OK) {
                item.command = IDEAS_DOWNLOAD;
                xSemaphoreTake(ctx->lock, portMAX_DELAY);
                ctx->state->total = item.pending->package.total_size;
                ctx->state->received = 0;
                xSemaphoreGive(ctx->lock);
                err = ideas_run_local(ctx, &item);
                app_storage_release();
            }
        }
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        for (size_t n = 0; n < ctx->state->count; ++n)
            if (!strcmp(ctx->state->items[n].code, item.code)) ctx->state->items[n].queued = false;
        xSemaphoreGive(ctx->lock);
        ideas_free_pending(item.pending);
    }
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->state->preview_id = 0;
    xSemaphoreGive(ctx->lock);
    if (err == ESP_OK) ideas_message(ctx, "Ideas up to date", "Saved copies are ready to open");
    (void)job;
    return err;
}
