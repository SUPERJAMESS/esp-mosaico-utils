// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

/* Only the large snapshot occupies PSRAM; mutex ownership stays internal. */
static EXT_RAM_BSS_ATTR factory_ideas_snapshot_t s_snapshot_storage;
static ideas_context_t s_ideas = {.state = &s_snapshot_storage};

esp_err_t factory_ideas_confirm(uint32_t request_id, bool download) {
    ideas_context_t *ctx = &s_ideas;
    if (!atomic_load(&ctx->initialized) || !request_id)
        return ESP_ERR_INVALID_STATE;
    job_t *job = calloc(1, sizeof(*job));
    if (!job)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (ctx->state->busy || !ctx->state->preview_ready || !ctx->pending ||
        request_id != ctx->state->preview_id || (download && ctx->pending->view_only)) {
        xSemaphoreGive(ctx->lock);
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    job->command = download && !ctx->pending->already_downloaded ? IDEAS_DOWNLOAD : IDEAS_DISMISS;
    job->pending = ctx->pending;
    strcpy(job->code, ctx->pending->package.code);
    ctx->state->busy = true;
    ctx->state->preview_ready = false;
    ctx->state->command = job->command;
    ctx->state->terminal = ctx->state->failed = false;
    ctx->state->cancelled = false;
    ctx->state->return_library = download && ctx->pending->already_downloaded;
    /* The root is already in memory from preview, not part of this transfer. */
    ctx->state->total = ctx->pending->package.total_size - ctx->pending->package.root_size;
    ctx->state->received = 0;
    ctx->state->phase = IDEAS_PHASE_DOWNLOAD;
    ctx->state->files_verified = 0;
    ctx->state->file_count = ctx->pending->package.file_count;
    ctx->cancel_requested = false;
    ctx->state->cancellable = job->command == IDEAS_DOWNLOAD;
    ctx->pending = NULL;
    esp_err_t err = ideas_launch_locked(ctx, job);
    if (err != ESP_OK) {
        ctx->pending = job->pending;
        ctx->state->preview_ready = true;
        ctx->state->busy = false;
    } else {
        ctx->state->preview_id = 0;
        ++ctx->state->revision;
    }
    xSemaphoreGive(ctx->lock);
    if (err != ESP_OK)
        free(job);
    return err;
}

esp_err_t factory_ideas_cancel(void) {
    ideas_context_t *ctx = &s_ideas;
    if (!atomic_load(&ctx->initialized))
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    uint32_t ready = ctx->state->preview_ready ? ctx->state->preview_id : 0;
    if (ctx->state->busy && ctx->state->cancellable) {
        ctx->cancel_requested = true;
        ctx->state->cancellable = false;
        snprintf(ctx->state->title, sizeof(ctx->state->title), "Cancelling...");
        snprintf(ctx->state->detail, sizeof(ctx->state->detail), "Please wait");
    } else if (ctx->state->busy && !ctx->state->resolving && !ctx->cancel_requested) {
        xSemaphoreGive(ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ready && ctx->state->resolving) {
        ctx->state->preview_id = 0;
        ctx->state->resolving = false;
        ctx->state->terminal = false;
    }
    xSemaphoreGive(ctx->lock);
    return ready ? factory_ideas_confirm(ready, false) : ESP_OK;
}

esp_err_t factory_ideas_request(factory_ideas_command_t command, const char *code) {
    ideas_context_t *ctx = &s_ideas;
    if (!atomic_load(&ctx->initialized))
        return ESP_ERR_INVALID_STATE;
    bool no_code = command == IDEAS_SCAN || command == IDEAS_RELOCALIZE || command == IDEAS_SAVE_DEVELOPMENT || command == IDEAS_UPDATE_ALL || command == IDEAS_RUN_DEVELOPMENT || command == IDEAS_REFRESH_UPDATES || command == IDEAS_STORAGE_INFO;
    if (command < IDEAS_SCAN || command > IDEAS_STORAGE_INFO || command == IDEAS_DOWNLOAD ||
        command == IDEAS_DISMISS || command == IDEAS_ROW || (!no_code && !ideas_local_code_valid(code)) ||
        ((command == IDEAS_CHECK || command == IDEAS_RESOLVE) && !ideas_code_valid(code)))
        return ESP_ERR_INVALID_ARG;
    job_t *job = calloc(1, sizeof(*job));
    if (!job)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (ctx->state->busy || ctx->pending) {
        xSemaphoreGive(ctx->lock);
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    if (command == IDEAS_START) {
        bool boot = false;
        for (size_t i = 0; i < ctx->state->count; i++)
            boot |= ctx->state->items[i].boot && !strcmp(ctx->state->items[i].code, code);
        xSemaphoreGive(ctx->lock);
        free(job);
        if (!boot)
            return ESP_ERR_INVALID_STATE;
        esp_restart();
        return ESP_OK;
    }
    job->command = command;
    if (!no_code)
        strcpy(job->code, code);
    ctx->state->busy = true;
    ctx->state->command = command;
    ctx->cancel_requested = false;
    ctx->state->cancellable = command == IDEAS_OPEN;
    ctx->state->batch = command == IDEAS_UPDATE_ALL;
    ctx->state->scanning = command == IDEAS_SCAN || command == IDEAS_RELOCALIZE;
    if (command == IDEAS_RESOLVE || command == IDEAS_CHECK || command == IDEAS_PREVIEW) {
        if (++ctx->request_id == 0)
            ++ctx->request_id;
        ctx->state->preview_id = job->request_id = ctx->request_id;
        ctx->state->resolving = true;
        ctx->state->preview_ready = false;
        ctx->state->return_library = false;
        ctx->state->already_downloaded = ctx->state->view_only = ctx->state->custom_display =
            ctx->state->has_icon = false;
        memset(&ctx->state->preview, 0, sizeof(ctx->state->preview));
    }
    if (command != IDEAS_SCAN && command != IDEAS_RELOCALIZE && command != IDEAS_STORAGE_INFO) {
        ctx->state->terminal = ctx->state->failed = false;
        ctx->state->cancelled = false;
        ctx->state->total = ctx->state->received = 0;
        ctx->state->phase = IDEAS_PHASE_NONE;
        ctx->state->files_verified = ctx->state->file_count = 0;
        snprintf(ctx->state->title, sizeof(ctx->state->title), "%s",
                 command == IDEAS_INSTALL || command == IDEAS_OPEN ? "Setting boot application" : "Loading app information");
        snprintf(ctx->state->detail, sizeof(ctx->state->detail), "Please wait");
        ++ctx->state->revision;
    }
    esp_err_t err = ideas_launch_locked(ctx, job);
    if (err != ESP_OK) {
        ctx->state->busy = ctx->state->scanning = ctx->state->resolving = false;
        ctx->state->preview_id = 0;
    }
    xSemaphoreGive(ctx->lock);
    if (err != ESP_OK)
        free(job);
    return err;
}

esp_err_t factory_ideas_snapshot(factory_ideas_snapshot_t *out) {
    ideas_context_t *ctx = &s_ideas;
    if (!out || !atomic_load(&ctx->initialized))
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(ctx->lock, 0) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    *out = (*ctx->state);
    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

void *factory_ideas_copy_icon(const char *code, size_t *size) {
    ideas_context_t *ctx = &s_ideas;
    if (!code || !size || !atomic_load(&ctx->initialized))
        return NULL;
    *size = 0;
    if (xSemaphoreTake(ctx->lock, 0) != pdTRUE)
        return NULL;
    void *copy = NULL;
    for (size_t i = 0; i < ctx->state->count; i++)
        if (!strcmp(code, ctx->state->items[i].code) && ctx->icons[i].data) {
            copy = malloc(ctx->icons[i].size);
            if (copy) {
                memcpy(copy, ctx->icons[i].data, ctx->icons[i].size);
                *size = ctx->icons[i].size;
            }
            break;
        }
    xSemaphoreGive(ctx->lock);
    return copy;
}

void *factory_ideas_copy_preview(uint32_t request_id, unsigned slot, size_t *size) {
    ideas_context_t *ctx = &s_ideas;
    if (!size || slot > 2 || !atomic_load(&ctx->initialized))
        return NULL;
    *size = 0;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    const icon_t *asset = NULL;
    if (ctx->pending && ctx->state->preview_ready && ctx->state->preview_id == request_id) {
        if (slot < 2)
            asset = &ctx->pending->pixels[slot];
        else {
            int index = ideas_asset_index(&ctx->pending->package, ctx->pending->package.icon);
            if (index >= 0)
                asset = &ctx->pending->assets[index];
        }
    }
    void *copy = asset && asset->data ? malloc(asset->size) : NULL;
    if (copy) {
        memcpy(copy, asset->data, asset->size);
        *size = asset->size;
    }
    xSemaphoreGive(ctx->lock);
    return copy;
}

esp_err_t factory_ideas_init(void) {
    return ideas_worker_init(&s_ideas);
}

void *factory_ideas_copy_title(const char *code, size_t *size) {
    ideas_context_t *ctx = &s_ideas;
    if (!ideas_code_valid(code) || !size || !atomic_load(&ctx->initialized))
        return NULL;
    *size = 0;
    if (xSemaphoreTake(ctx->lock, 0) != pdTRUE)
        return NULL;
    unsigned language = factory_locale_get();
    for (unsigned i = 0; i < 8; ++i) {
        if (!strcmp(ctx->titles[i].code, code) && ctx->titles[i].language == language) {
            icon_t *image = &ctx->titles[i].image;
            void *copy = image->data ? malloc(image->size) : NULL;
            if (copy) {
                memcpy(copy, image->data, image->size);
                *size = image->size;
            }
            ctx->titles[i].stamp = ++ctx->title_clock;
            xSemaphoreGive(ctx->lock);
            return copy;
        }
    }
    if (!ctx->state->busy && !ctx->pending) {
        bool known = false;
        for (unsigned i = 0; i < ctx->state->count; ++i)
            known |= !strcmp(ctx->state->items[i].code, code);
        job_t *job = known ? calloc(1, sizeof(*job)) : NULL;
        if (job) {
            job->command = IDEAS_ROW;
            strcpy(job->code, code);
            job->request_id = ctx->state->preview_id;
            ctx->state->busy = true;
            ctx->state->command = IDEAS_ROW;
            if (ideas_launch_locked(ctx, job) != ESP_OK) {
                free(job);
                ctx->state->busy = false;
            }
        }
    }
    xSemaphoreGive(ctx->lock);
    return NULL;
}
