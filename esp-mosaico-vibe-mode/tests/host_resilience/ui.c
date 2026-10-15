#include "sdk.h"
#include SOURCE

static vui_timer_cb_t timer_callback;
static void *timer_context;
static bool fail_timer;
static esp_err_t init_result, open_result;
static unsigned opens;
static unsigned char state_storage;
static vibe_ui_t *state = (vibe_ui_t *)&state_storage;
static const vibe_services_t services;

void *vui_timer_create(vui_handle_t ui, unsigned period,
                           vui_timer_cb_t callback, void *ctx)
{
    if (fail_timer) return NULL;
    timer_callback = callback; timer_context = ctx; return (void *)1;
}
vui_err_t vibe_ui_create(vui_handle_t ui, const vibe_services_t *svc, vibe_ui_t **out)
{
    *out = state;
    assert(svc != &services); /* Caller storage is copied. */
    return init_result;
}
int vibe_ui_open_bridge(vibe_ui_t *s) { assert(s == state); ++opens; return open_result; }
static void render(void) { timer_callback((void *)1, timer_context); }
static void reset(void)
{
    if (s_dispatch.initialized) vSemaphoreDelete(s_dispatch.initialized);
    if (s_dispatch.call_lock) vSemaphoreDelete(s_dispatch.call_lock);
    if (s_dispatch.requests) vQueueDelete(s_dispatch.requests);
    if (s_dispatch.responses) vQueueDelete(s_dispatch.responses);
    s_dispatch.initialized = s_dispatch.call_lock = NULL;
    s_dispatch.requests = s_dispatch.responses = NULL;
    s_dispatch.state = NULL; s_dispatch.started = false; s_dispatch.init_attempted = false; atomic_store(&s_dispatch.ready, false);
    mock_wait = NULL; opens = 0; init_result = open_result = ESP_OK;
    assert(mock_allocations == 0);
}
int main(void)
{
    for (int i = 0; i < 4; ++i) {
        mock_alloc_fail_after = i;
        assert(factory_ui_dispatch_start((void *)1, &services) == ESP_ERR_NO_MEM);
        assert(mock_allocations == 0);
    }
    mock_alloc_fail_after = -1; fail_timer = true;
    assert(factory_ui_dispatch_start((void *)1, &services) == ESP_ERR_NO_MEM);
    assert(mock_allocations == 0);
    fail_timer = false;
    assert(factory_ui_dispatch_open_bridge() == ESP_ERR_INVALID_STATE);

    /* Initialization completes before handing ownership to the render task. */
    mock_now = 0;
    assert(factory_ui_dispatch_start((void *)1, &services) == ESP_OK);
    assert(mock_now == 0);
    mock_wait = render; open_result = 123;
    assert(factory_ui_dispatch_open_bridge() == 123 && opens == 1);

    mock_wait = NULL;
    int64_t before = mock_now;
    assert(factory_ui_dispatch_open_bridge() == ESP_ERR_TIMEOUT);
    assert(mock_now - before == 2000000);
    render(); /* An expired queued request must not open a page later. */
    assert(opens == 1);

    /* A previous callback can finish after its caller timed out. Its result
     * must neither complete a new request nor extend that request's budget. */
    ui_response_t stale = {.id = s_dispatch.next_request, .result = 456};
    xQueueOverwrite(s_dispatch.responses, &stale);
    before = mock_now;
    assert(factory_ui_dispatch_open_bridge() == ESP_ERR_TIMEOUT);
    assert(mock_now - before == 2000000 && opens == 1);
    stale.id = s_dispatch.next_request;
    xQueueOverwrite(s_dispatch.responses, &stale);
    mock_wait = render; open_result = 789;
    assert(factory_ui_dispatch_open_bridge() == 789 && opens == 2);

    /* Contention is bounded as well, without enqueueing another request. */
    assert(xSemaphoreTake(s_dispatch.call_lock, 0));
    mock_wait = NULL; before = mock_now;
    assert(factory_ui_dispatch_open_bridge() == ESP_ERR_TIMEOUT);
    assert(mock_now - before == 2000000 && opens == 2);
    xSemaphoreGive(s_dispatch.call_lock);
    reset();

    /* Failed initialization never accepts requests that cannot be serviced. */
    init_result = ESP_ERR_NO_MEM; mock_wait = render;
    assert(factory_ui_dispatch_start((void *)1, &services) == ESP_ERR_NO_MEM);
    assert(factory_ui_dispatch_open_bridge() == ESP_ERR_INVALID_STATE);
    reset();
    puts("UI startup, expired requests, late replies and retries passed");
}
