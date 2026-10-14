#include "sdk.h"

int64_t mock_now;
void (*mock_wait)(void);
int mock_alloc_fail_after = -1;
unsigned mock_allocations;
struct mock_queue { size_t size; bool full; uint8_t value[32]; };

const char *esp_err_to_name(esp_err_t err) { return "injected"; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
int64_t esp_timer_get_time(void) { return mock_now; }
QueueHandle_t xQueueCreate(unsigned count, size_t size)
{
    assert(count == 1 && size <= 32);
    if (mock_alloc_fail_after == 0) return NULL;
    if (mock_alloc_fail_after > 0) --mock_alloc_fail_after;
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q); ++mock_allocations; q->size = size; return q;
}
int xQueueOverwrite(QueueHandle_t q, const void *value)
{
    memcpy(q->value, value, q->size); q->full = true; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *value, TickType_t wait)
{
    if (!q->full && wait) {
        assert(wait != portMAX_DELAY); /* Never simulate an unbounded wait. */
        if (mock_wait) mock_wait();
        if (!q->full) mock_now += (int64_t)wait * 10000;
    }
    if (!q->full) return pdFALSE;
    if (value) memcpy(value, q->value, q->size);
    q->full = false; return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { assert(mock_allocations); --mock_allocations; free(q); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xQueueCreate(1, 0); }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t s = xSemaphoreCreateBinary(); if (s) s->full = true; return s;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return xQueueReceive(s, NULL, wait); }
int xSemaphoreGive(SemaphoreHandle_t s) { s->full = true; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { vQueueDelete(s); }
