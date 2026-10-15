// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The platform serializes access. Keep gesture edges, coalesce movement, and
 * retain acquisition time so a delayed batch cannot manufacture fling speed. */
enum { VIBE_INPUT_DOWN, VIBE_INPUT_MOVE, VIBE_INPUT_UP, VIBE_INPUT_CAPACITY = 32 };
typedef struct {
    int16_t x, y;
    uint8_t kind, source;
    uint64_t time_ms;
} vibe_input_t;
typedef struct {
    vibe_input_t events[VIBE_INPUT_CAPACITY];
    unsigned count;
} vibe_input_queue_t;

static inline bool vibe_input_push(vibe_input_queue_t *queue, vibe_input_t event)
{
    if (queue->count >= 2 && event.kind == VIBE_INPUT_MOVE) {
        vibe_input_t *last = &queue->events[queue->count - 1];
        const vibe_input_t *before = &queue->events[queue->count - 2];
        /* Keep direction changes: an out-and-back drag must never collapse
         * into a tap at its starting position. */
        if (last->kind == VIBE_INPUT_MOVE && last->source == event.source &&
            before->source == event.source && before->kind != VIBE_INPUT_UP &&
            (last->x - before->x) * (event.x - last->x) >= 0 &&
            (last->y - before->y) * (event.y - last->y) >= 0) {
            *last = event;
            return true;
        }
    }
    if (queue->count == VIBE_INPUT_CAPACITY) {
        unsigned i = 0;
        while (i < queue->count && queue->events[i].kind != VIBE_INPUT_MOVE) ++i;
        if (i == queue->count) return false; /* Apply backpressure to edges. */
        memmove(&queue->events[i], &queue->events[i + 1],
                (queue->count - i - 1) * sizeof(event));
        --queue->count;
    }
    queue->events[queue->count++] = event;
    return true;
}

static inline bool vibe_input_pop(vibe_input_queue_t *queue, vibe_input_t *event)
{
    if (!queue->count) return false;
    *event = queue->events[0];
    --queue->count;
    memmove(queue->events, queue->events + 1, queue->count * sizeof(*event));
    return true;
}
