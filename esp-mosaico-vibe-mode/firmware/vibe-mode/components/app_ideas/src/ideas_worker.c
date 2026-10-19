// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

void ideas_message(ideas_context_t *ctx, const char *title, const char *detail) {
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    snprintf(ctx->state->title, sizeof(ctx->state->title), "%s", title);
    snprintf(ctx->state->detail, sizeof(ctx->state->detail), "%s", detail);
    xSemaphoreGive(ctx->lock);
}

static void run_job(job_t *job) {
    ideas_context_t *ctx = job->owner;
    factory_io_metrics_t metrics = {.started = factory_io_now()};
#ifdef ESP_PLATFORM
    /* INTERNAL includes LP/RTC RAM on ESP32-S31. Record the actual stack and
     * largest HP SRAM block so slow-stack fallback is visible in field logs. */
    ESP_LOGI("io_perf", "task_start command=%u core=%u stack=%p dram_largest=%u rtc_largest=%u",
             (unsigned)job->command, (unsigned)xPortGetCoreID(), (void *)&job,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT));
#endif
    bool remote = job->command == IDEAS_RESOLVE || job->command == IDEAS_CHECK;
    bool preparing = remote || job->command == IDEAS_PREVIEW;
    esp_err_t err = ESP_OK;
    if ((job->command == IDEAS_UPDATE_ALL || job->command == IDEAS_REFRESH_UPDATES))
        err = ideas_update_all(ctx, job);
    else if (job->command == IDEAS_RUN_DEVELOPMENT)
        err = ideas_run_development();
    else if (remote)
        err = ideas_prepare_download(ctx, job);
    else {
        if (job->command != IDEAS_DISMISS) {
            err = app_storage_acquire();
            if (err == ESP_OK) {
                err = ideas_run_local(ctx, job);
                app_storage_release();
            }
        }
    }
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    bool stale = preparing && job->request_id != ctx->state->preview_id;
    if (remote && !stale && err == ESP_OK) {
        for (size_t i = 0; i < ctx->state->count; ++i)
            if (!strcmp(ctx->state->items[i].code, job->code)) {
                factory_idea_t *item = &ctx->state->items[i];
                item->update_available =
                    strcmp(item->release_id, job->pending->package.release_id) != 0;
                snprintf(item->latest_version, sizeof(item->latest_version), "%s",
                         job->pending->package.version);
                ++ctx->state->generation;
                break;
            }
    }
    bool ready = preparing && !stale && err == ESP_OK && !job->up_to_date;
    if (ready) {
        ctx->pending = job->pending;
        job->pending = NULL;
        ideas_preview_item(&ctx->state->preview, &ctx->pending->package);
        ctx->state->already_downloaded = ctx->pending->already_downloaded;
        ctx->state->preview_ready = true;
        ctx->state->view_only = ctx->pending->view_only;
        ctx->state->custom_display = ctx->pending->pixels[0].data != NULL;
        ctx->state->has_icon = ctx->pending->package.icon[0] != 0;
    }
    ctx->state->busy = false;
    ctx->state->cancellable = false;
    ctx->state->batch = false;
    ctx->state->batch_code[0] = 0;
    for (size_t i = 0; i < ctx->state->count; ++i) ctx->state->items[i].queued = false;
    ctx->state->scanning = false;
    if (preparing)
        ctx->state->resolving = false;
    if (job->command == IDEAS_REFRESH_UPDATES) {
        ctx->state->terminal = false;
    } else if (job->command == IDEAS_SCAN || job->command == IDEAS_RELOCALIZE ||
               job->command == IDEAS_STORAGE_INFO)
        ctx->state->scan_error = err;
    else if (!stale && !ready && job->command != IDEAS_DISMISS && job->command != IDEAS_ROW &&
             (job->command != IDEAS_PREVIEW || err != ESP_OK)) {
        ctx->state->terminal = true;
        ctx->state->failed = err != ESP_OK;
        ctx->state->error = err;
        if (preparing)
            ctx->state->preview_id = 0;
        if (err != ESP_OK) {
            snprintf(ctx->state->title, sizeof(ctx->state->title), "%s failed",
                     preparing                       ? "App information"
                     : job->command == IDEAS_INSTALL ? "Installation"
                     : job->command == IDEAS_OPEN    ? "Open"
                                                     : "Download");
            snprintf(ctx->state->detail, sizeof(ctx->state->detail), "%s (0x%x).\n%s",
                     esp_err_to_name(err), (unsigned)err,
                     factory_locale_get() ? "Return home and try again." : "返回主界面后重试。");
        }
        if (ctx->cancel_requested) {
            ctx->state->failed = false;
            ctx->state->cancelled = true;
            snprintf(ctx->state->title, sizeof(ctx->state->title), "Cancelled");
            snprintf(ctx->state->detail, sizeof(ctx->state->detail), "Previously saved Ideas are unchanged");
        }
        if (err == FACTORY_IDEAS_ERR_NO_COMPATIBLE_RELEASE && !ctx->cancel_requested) {
            snprintf(ctx->state->title, sizeof(ctx->state->title), "%s",
                     factory_locale_get() ? "No compatible version" : "没有兼容版本");
            snprintf(ctx->state->detail, sizeof(ctx->state->detail), "%s",
                     factory_locale_get() ? "This Idea has no release compatible with this device."
                                          : "此 Idea 暂无与当前设备兼容的发布版本。");
        } else if (err == FACTORY_IDEAS_ERR_STORAGE_FULL) {
            snprintf(ctx->state->title, sizeof(ctx->state->title), "Not enough space");
            snprintf(ctx->state->detail, sizeof(ctx->state->detail), "Free space or delete an Idea, then retry.");
        } else if (err == ESP_ERR_NOT_FOUND && job->command == IDEAS_RESOLVE) {
            snprintf(ctx->state->title, sizeof(ctx->state->title), "No Idea with this ID");
        }
        ++ctx->state->revision;
    }
    xSemaphoreGive(ctx->lock);
    ideas_free_pending(job->pending);
    char operation[20];
    snprintf(operation, sizeof(operation), "%u/%s", (unsigned)job->command, job->code);
    factory_io_report("operation", operation, &metrics, NULL, 0, err);
#ifdef ESP_PLATFORM
    ESP_LOGI("io_perf", "memory internal_free=%u internal_largest=%u internal_min=%u psram_free=%u stack_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    free(job);
}

static void ideas_worker(void *arg) {
    ideas_context_t *ctx = arg;
    job_t *job;
    for (;;) {
        if (xQueueReceive(ctx->jobs, &job, portMAX_DELAY) == pdTRUE)
            run_job(job);
    }
}

/* Called only from serialized startup. Publish initialized last, so UI calls
 * cannot access partial resources while an allocation fails and rolls back. */
esp_err_t ideas_worker_init(ideas_context_t *ctx) {
    if (atomic_load(&ctx->initialized))
        return ESP_OK;
    job_t *initial = calloc(1, sizeof(*initial));
    if (!initial)
        return ESP_ERR_NO_MEM;
    ctx->lock = xSemaphoreCreateMutex();
    ctx->jobs = xQueueCreate(1, sizeof(job_t *));
    if (!ctx->lock || !ctx->jobs)
        goto fail;
    initial->owner = ctx;
    initial->command = IDEAS_SCAN;
    initial->startup = true;
    if (xQueueSend(ctx->jobs, &initial, 0) != pdTRUE)
        goto fail;
    ctx->state->busy = ctx->state->scanning = true;
    ctx->state->command = IDEAS_SCAN;
    /* On S31 INTERNAL alone also admits slow LP/RTC RAM. DMA capability
     * restricts this lifetime stack to HP SRAM, with no slower fallback.
     * Create after Wi-Fi startup, before normal UI jobs fragment the heap. */
    if (xTaskCreatePinnedToCoreWithCaps(ideas_worker, "ideas", 16384, ctx, 3,
                                       &ctx->worker, tskNO_AFFINITY,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA) != pdPASS)
        goto fail;
    atomic_store(&ctx->initialized, true);
    return ESP_OK;
fail:
    if (ctx->jobs)
        vQueueDelete(ctx->jobs);
    if (ctx->lock)
        vSemaphoreDelete(ctx->lock);
    ctx->jobs = NULL;
    ctx->lock = NULL;
    ctx->worker = NULL;
    ctx->state->busy = ctx->state->scanning = false;
    free(initial);
    return ESP_ERR_NO_MEM;
}

esp_err_t ideas_launch_locked(ideas_context_t *ctx, job_t *job) {
    job->owner = ctx;
    return xQueueSend(ctx->jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
