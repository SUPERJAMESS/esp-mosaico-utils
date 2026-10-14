// SPDX-License-Identifier: Apache-2.0
#include "factory_ui_dispatch.h"

#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define UI_START_TIMEOUT_MS 5000
#define UI_REQUEST_TIMEOUT_MS 2000

typedef struct {
    uint32_t id;
    int64_t deadline;
} ui_request_t;

typedef struct {
    uint32_t id;
    esp_err_t result;
} ui_response_t;

typedef struct {
    vibe_ui_t *state;
    vibe_services_t services;
    SemaphoreHandle_t initialized, call_lock;
    QueueHandle_t requests, responses;
    bool init_attempted, started;
    atomic_bool ready;
    esp_err_t init_result;
    uint32_t next_request;
} ui_dispatch_context_t;
static ui_dispatch_context_t s_dispatch;

static TickType_t remaining_ticks(int64_t deadline)
{
    const int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0)
        return 0;
    const TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
    return ticks > 0 ? ticks : 1;
}

static void process_commands(esp_gsp_handle_t ui, void *argument)
{
    ui_dispatch_context_t *ctx = argument;
    if (!ctx->init_attempted) {
        ctx->init_attempted = true;
        ctx->init_result = vibe_ui_create(ui, &ctx->services, &ctx->state);
        atomic_store(&ctx->ready, ctx->init_result == ESP_OK);
        xSemaphoreGive(ctx->initialized);
    }
    if (!atomic_load(&ctx->ready))
        return;
    ui_request_t request;
    if (xQueueReceive(ctx->requests, &request, 0) == pdTRUE &&
        esp_timer_get_time() < request.deadline) {
        const ui_response_t response = {
            .id = request.id,
            .result = vibe_ui_open_bridge(ctx->state),
        };
        /* A timed-out caller owns no queued storage. A late response carries
         * its request ID and cannot complete the next caller's request. */
        xQueueOverwrite(ctx->responses, &response);
    }
}

esp_err_t factory_ui_dispatch_start(esp_gsp_handle_t ui, const vibe_services_t *services)
{
    ui_dispatch_context_t *ctx = &s_dispatch;
    if (ctx->started)
        return ESP_ERR_INVALID_STATE;
    ctx->initialized = xSemaphoreCreateBinary();
    ctx->call_lock = xSemaphoreCreateMutex();
    ctx->requests = xQueueCreate(1, sizeof(ui_request_t));
    ctx->responses = xQueueCreate(1, sizeof(ui_response_t));
    if (!ctx->initialized || !ctx->call_lock || !ctx->requests || !ctx->responses)
        goto fail;
    ctx->started = true;
    ctx->services = *services;
    if (!esp_gsp_timer_create(ui, 50, process_commands, ctx))
        goto fail;
    /* Once registered, the callback owns these resources, including after a
     * timeout. It may complete later without accessing a caller's stack. */
    if (xSemaphoreTake(ctx->initialized, pdMS_TO_TICKS(UI_START_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ctx->init_result;
fail:
    if (ctx->initialized)
        vSemaphoreDelete(ctx->initialized);
    if (ctx->call_lock)
        vSemaphoreDelete(ctx->call_lock);
    if (ctx->requests)
        vQueueDelete(ctx->requests);
    if (ctx->responses)
        vQueueDelete(ctx->responses);
    ctx->initialized = ctx->call_lock = NULL;
    ctx->requests = ctx->responses = NULL;
    ctx->state = NULL;
    ctx->started = false;
    return ESP_ERR_NO_MEM;
}

esp_err_t factory_ui_dispatch_open_bridge(void)
{
    ui_dispatch_context_t *ctx = &s_dispatch;
    if (!atomic_load(&ctx->ready))
        return ESP_ERR_INVALID_STATE;
    const int64_t deadline = esp_timer_get_time() + UI_REQUEST_TIMEOUT_MS * 1000LL;
    if (xSemaphoreTake(ctx->call_lock, remaining_ticks(deadline)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const ui_request_t request = {.id = ++ctx->next_request, .deadline = deadline};
    /* Replace a request that expired while the render task was stalled. */
    xQueueOverwrite(ctx->requests, &request);
    esp_err_t result = ESP_ERR_TIMEOUT;
    ui_response_t response;
    TickType_t wait;
    while ((wait = remaining_ticks(deadline)) != 0 &&
           xQueueReceive(ctx->responses, &response, wait) == pdTRUE) {
        if (response.id == request.id) {
            result = response.result;
            break;
        }
    }
    xSemaphoreGive(ctx->call_lock);
    return result;
}
