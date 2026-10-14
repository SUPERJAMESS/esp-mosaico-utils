// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

bool bridge_event_with_timeout(bridge_context_t *ctx, const char *state, unsigned progress,
                               esp_err_t error, int timeout_ms)
{
    bridge_set_state(ctx, state, progress, error);
    char path[256];
    bridge_path_session(ctx, path, "/progress");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "phase", state);
    cJSON_AddNumberToObject(j, "progress", progress);
    if (error != ESP_OK)
        cJSON_AddStringToObject(j, "error", esp_err_to_name(error));
    cJSON *r = bridge_request_with_response(ctx, path, j, &ctx->main_response, &ctx->main_client,
                                            timeout_ms, false);
    bool ok = r != NULL && !strcmp(bridge_str(r, "phase"), state);
    if (!ok)
        ESP_LOGW("iris_bridge", "phase %s failed: http=%d response=%s", state,
                 ctx->main_response.status, r ? "unexpected phase" : "missing/invalid");
    if (atomic_load(&ctx->stop_requested) ||
        (r && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "cancel_requested"))))
        atomic_store(&ctx->cancelled, true);
    cJSON_Delete(r);
    cJSON_Delete(j);
    return ok;
}

bool bridge_terminal_event(bridge_context_t *ctx, const char *state, unsigned progress,
                           esp_err_t error)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (bridge_event_with_timeout(ctx, state, progress, error, 2000))
            return true;
        if (ctx->main_response.status == 401 || ctx->main_response.status == 404 ||
            ctx->main_response.status == 410)
            return true;
        if (attempt < 2)
            vTaskDelay(pdMS_TO_TICKS(250U << attempt));
    }
    ESP_LOGW("iris_bridge", "terminal phase %s delivery unconfirmed after retries", state);
    return false;
}

bool bridge_should_cancel(bridge_context_t *ctx)
{
    return atomic_load(&ctx->stop_requested) || atomic_load(&ctx->cancelled) ||
           (ctx->write_deadline && esp_timer_get_time() >= ctx->write_deadline);
}

void bridge_notify_progress(bridge_context_t *ctx, unsigned progress)
{
    bridge_set_state(ctx, "WRITING", progress, ESP_OK);
    atomic_store(&ctx->telemetry_progress, progress);
    atomic_fetch_add(&ctx->telemetry_revision, 1);
}

void bridge_cancel_poll_run(void *unused)
{
    bridge_context_t *ctx = unused;

    http_response_t response = {0};
    esp_http_client_handle_t client = NULL;
    unsigned interval = 5, sent = 0;
    int64_t next_poll = esp_timer_get_time() + 5000000;
    int64_t next_progress = esp_timer_get_time() + 1000000;
    while (!atomic_load(&ctx->cancel_poll_stop) && !bridge_should_cancel(ctx)) {
        unsigned revision = atomic_load(&ctx->telemetry_revision);
        bool telemetry = revision != sent && esp_timer_get_time() >= next_progress;
        if (!telemetry && esp_timer_get_time() < next_poll) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char path[256];
        bridge_path_session(ctx, path, telemetry ? "/progress" : "/poll");
        cJSON *body = NULL;
        if (telemetry) {
            body = cJSON_CreateObject();
            if (!body) {
                next_progress = esp_timer_get_time() + 1000000;
                continue;
            }
            cJSON_AddStringToObject(body, "phase", "WRITING");
            cJSON_AddNumberToObject(body, "progress", atomic_load(&ctx->telemetry_progress));
        }
        const int64_t started = esp_timer_get_time();
        cJSON *r = bridge_request_with_response(ctx, path, body, &response, &client, 5000, true);
        cJSON_Delete(body);
        const bool ok = r != NULL && (!telemetry || !strcmp(bridge_str(r, "phase"), "WRITING"));
        if (r) {
            const cJSON *flash = telemetry ? r : cJSON_GetObjectItemCaseSensitive(r, "flash");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(flash, "cancel_requested"))) {
                ESP_LOGW("iris_bridge", "remote cancellation received via %s",
                         telemetry ? "progress" : "poll");
                atomic_store(&ctx->cancelled, true);
            }
        } else if (response.status == 401 || response.status == 404 || response.status == 410) {
            ESP_LOGW("iris_bridge", "write authority ended: http=%d", response.status);
            atomic_store(&ctx->cancelled, true);
        }
        cJSON_Delete(r);
        if (ok && telemetry) {
            sent = revision;
            atomic_store(&ctx->writing_acknowledged, true);
        }
        interval = ok ? 5 : (interval < 15 ? interval * 2 : 30);
        if (response.retry_after_seconds > interval)
            interval = response.retry_after_seconds;
        next_poll = esp_timer_get_time() + (int64_t)interval * 1000000;
        next_progress = esp_timer_get_time() + (int64_t)(ok ? 1 : interval) * 1000000;
        if (!ok && !atomic_load(&ctx->cancel_poll_stop) && !bridge_should_cancel(ctx))
            ESP_LOGW("iris_bridge", "%s failed: http=%d elapsed=%lld ms; retry in %u s",
                     telemetry ? "telemetry" : "cancel poll", response.status,
                     (long long)((esp_timer_get_time() - started) / 1000), interval);
    }
    bridge_close_client(&client);
    atomic_store(&ctx->cancel_poll_running, false);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t bridge_cancel_poll_start(bridge_context_t *ctx)
{
    atomic_store(&ctx->cancel_poll_stop, false);
    atomic_store(&ctx->telemetry_revision, 0);
    atomic_store(&ctx->writing_acknowledged, false);
    atomic_store(&ctx->cancel_poll_running, true);
    if (xTaskCreateWithCaps(bridge_cancel_poll_run, "iris_cancel", 8192, ctx, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS)
        return ESP_OK;
    atomic_store(&ctx->cancel_poll_running, false);
    return ESP_ERR_NO_MEM;
}

void bridge_cancel_poll_join(bridge_context_t *ctx)
{
    atomic_store(&ctx->cancel_poll_stop, true);
    /* Let a bounded HTTP request unwind itself; deleting a task while it owns
     * TLS/HTTP allocations would leak memory or leave shared locks held. */
    while (atomic_load(&ctx->cancel_poll_running))
        vTaskDelay(pdMS_TO_TICKS(10));
}

bool bridge_authorize_commit(bridge_context_t *ctx)
{
    if (bridge_should_cancel(ctx))
        return false;
    bridge_set_state(ctx, "COMMITTING", 98, ESP_OK);
    char path[256];
    bridge_path_session(ctx, path, "/authorize");
    cJSON *body = cJSON_CreateObject();
    if (!body)
        return false;
    cJSON_AddStringToObject(body, "phase", "COMMITTING");
    cJSON_AddNumberToObject(body, "progress", 98);
    cJSON *r = bridge_request(ctx, path, body);
    bool ok = r && !strcmp(bridge_str(r, "phase"), "COMMITTING") &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "authorized"));
    if (!ok)
        ESP_LOGW("iris_bridge", "commit authorization failed: http=%d response=%s",
                 ctx->main_response.status, r ? "denied/malformed" : "missing/invalid");
    cJSON_Delete(r);
    cJSON_Delete(body);
    return ok;
}

void bridge_report_result_run(void *unused)
{
    bridge_context_t *ctx = unused;

    char path[256];
    bridge_path_session(ctx, path, "/progress");
    cJSON *body = cJSON_CreateObject();
    if (body) {
        cJSON_AddStringToObject(body, "phase", ctx->final_result == ESP_OK ? "DONE" : "FAILED");
        cJSON_AddNumberToObject(body, "progress", 100);
        if (ctx->final_result != ESP_OK)
            cJSON_AddStringToObject(body, "error", esp_err_to_name(ctx->final_result));
        cJSON *r = bridge_request_with_response(ctx, path, body, &ctx->main_response,
                                                &ctx->main_client, 1500, false);
        if (!r)
            ESP_LOGW("iris_bridge", "result delivery unconfirmed: http=%d; local result retained",
                     ctx->main_response.status);
        cJSON_Delete(r);
        cJSON_Delete(body);
    }
    bridge_close_client(&ctx->main_client);
    atomic_store(&ctx->report_running, false);
    vTaskDeleteWithCaps(NULL);
}

void bridge_report_result_bounded(bridge_context_t *ctx, esp_err_t result)
{
    ctx->final_result = result;
    bridge_set_state(ctx, result == ESP_OK ? "DONE" : "FAILED", 100, result);
    atomic_store(&ctx->report_running, true);
    const int64_t deadline = esp_timer_get_time() + 2000000;
    if (xTaskCreateWithCaps(bridge_report_result_run, "iris_result", 8192, ctx, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&ctx->report_running, false);
        ESP_LOGW("iris_bridge", "result reporter unavailable; local result retained");
        return;
    }
    while (atomic_load(&ctx->report_running) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (atomic_load(&ctx->report_running))
        ESP_LOGW("iris_bridge",
                 "result delivery deadline reached; rebooting with persisted result");
    /* Do not delete a task owning TLS locks, or mutate its token/session. */
}
