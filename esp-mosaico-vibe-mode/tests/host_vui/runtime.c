// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_ids.h"
#include "vibe_input_queue.h"
#include "vibe_home_motion.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
extern int vibe_pc_init(vui_handle_t ui);
extern void vibe_pc_deinit(vui_handle_t ui);
static unsigned releases, ticks;
static void released(void *data) { releases++; free(data); }
static void timer(vui_handle_t ui, void *ctx) { ticks++; assert(vui_timer_delete(ui,*(void **)ctx)==0); }
static void queue_contract(void)
{
    vibe_input_queue_t queue = {0};
    vibe_input_t event = {.x = 420, .y = 250, .kind = VIBE_INPUT_DOWN, .time_ms = 1000};
    assert(vibe_input_push(&queue, event));
    for (int x = 419; x >= 60; --x) {
        event.x = x; event.kind = VIBE_INPUT_MOVE; ++event.time_ms;
        assert(vibe_input_push(&queue, event));
    }
    event.kind = VIBE_INPUT_UP; ++event.time_ms;
    assert(vibe_input_push(&queue, event) && queue.count == 3);
    assert(vibe_input_pop(&queue, &event) && event.kind == VIBE_INPUT_DOWN && event.x == 420);
    assert(vibe_input_pop(&queue, &event) && event.kind == VIBE_INPUT_MOVE && event.x == 60 && event.time_ms == 1360);
    assert(vibe_input_pop(&queue, &event) && event.kind == VIBE_INPUT_UP);
    event = (vibe_input_t){.x = 100, .y = 200, .kind = VIBE_INPUT_DOWN};
    assert(vibe_input_push(&queue, event));
    event.kind = VIBE_INPUT_MOVE; event.x = 200;
    assert(vibe_input_push(&queue, event));
    event.x = 100;
    assert(vibe_input_push(&queue, event));
    event.kind = VIBE_INPUT_UP;
    assert(vibe_input_push(&queue, event) && queue.count == 4);
    while (vibe_input_pop(&queue, &event)) {}
    for (unsigned i = 0; i < VIBE_INPUT_CAPACITY; ++i) {
        event.kind = i & 1 ? VIBE_INPUT_UP : VIBE_INPUT_DOWN;
        event.time_ms = i;
        assert(vibe_input_push(&queue, event));
    }
    assert(!vibe_input_push(&queue, event));
    for (unsigned i = 0; i < VIBE_INPUT_CAPACITY; ++i)
        assert(vibe_input_pop(&queue, &event) && event.time_ms == i);
    assert(!vibe_input_pop(&queue, &event));
    /* Saturation discards a move before any gesture boundary, and movement
     * from different sources is never coalesced together. */
    for (unsigned i = 0; i < VIBE_INPUT_CAPACITY; ++i) {
        event.kind = VIBE_INPUT_MOVE; event.source = i & 1; event.time_ms = i;
        assert(vibe_input_push(&queue, event));
    }
    event.kind = VIBE_INPUT_UP;
    assert(vibe_input_push(&queue, event));
    assert(vibe_input_pop(&queue, &event) && event.time_ms == 1);
}
static void compare_full(vui_handle_t ui, uint16_t *frame)
{
    uint16_t *expected = malloc(480 * 480 * 2);
    assert(expected);
    memcpy(expected, frame, 480 * 480 * 2);
    vui_invalidate(ui);
    assert(vui_render(ui, frame));
    assert(!memcmp(expected, frame, 480 * 480 * 2));
    free(expected);
}
static void motion_contract(void)
{
    vibe_home_motion_t regular, delayed;
    vibe_home_motion_enter(&regular, 7000);
    vibe_home_motion_enter(&delayed, 7000);
    for (unsigned t = 0; t <= 30000; t += 40) {
        vibe_home_motion_sample(&regular, 7000 + t);
        for (unsigned i = 0; i < VIBE_HOME_DOT_COUNT; ++i) {
            const vui_dot_t *d = &regular.dots[i];
            assert(d->x - d->radius > 0 && d->x + d->radius < 120 * 16);
            assert(d->y - d->radius > 0 && d->y + d->radius < 120 * 16);
        }
    }
    /* A stalled render task reaches the same pose without replaying frames. */
    vibe_home_motion_sample(&delayed, 37000);
    assert(!memcmp(regular.dots, delayed.dots, sizeof(regular.dots)));
    vibe_home_motion_sample(&regular, 8000);
    vui_dot_t rest[VIBE_HOME_DOT_COUNT];
    memcpy(rest, regular.dots, sizeof(rest));
    vibe_home_motion_sample(&regular, 8000 + 12400);
    assert(!memcmp(rest, regular.dots, sizeof(rest)));
    /* The entrance, scan and repeated cycle joins converge to the idle pose. */
    for (unsigned join = 0; join < 3; ++join) {
        uint64_t time = 8000 + join * 12400;
        vibe_home_motion_sample(&regular, time - 1);
        for (unsigned i = 0; i < VIBE_HOME_DOT_COUNT; ++i) {
            assert(abs(regular.dots[i].x - rest[i].x) <= 1);
            assert(abs(regular.dots[i].radius - rest[i].radius) <= 1);
            assert(abs(regular.dots[i].brightness - rest[i].brightness) <= 2);
        }
    }
    vibe_home_motion_sample(&regular, 39000);
    for (unsigned t = 0; t < 500; t += 40) {
        vui_dot_t before[VIBE_HOME_DOT_COUNT];
        memcpy(before, regular.dots, sizeof(before));
        vibe_home_motion_tap(&regular, 39000 + t, 12 + t % 90, 60);
        vibe_home_motion_sample(&regular, 39000 + t);
        assert(!memcmp(before, regular.dots, sizeof(before)));
        vibe_home_motion_sample(&regular, 39040 + t);
    }
    vibe_home_motion_sample(&regular, 41000);
    vibe_home_motion_sample(&delayed, 41000);
    assert(!regular.ripple);
    assert(!memcmp(regular.dots, delayed.dots, sizeof(regular.dots)));
    vibe_home_motion_tap(&regular, 41000, -1, 60);
    assert(!regular.ripple);
    /* Large elapsed intervals are bounded and preserve the periodic phase. */
    vibe_home_motion_sample(&regular, 8000ULL + 12400ULL * 1000000000ULL);
    assert(!memcmp(rest, regular.dots, sizeof(rest)));
}
static void dot_rendering(uint16_t *frame)
{
    vui_handle_t ui;
    assert(vui_create(&ui) == VUI_OK);
    assert(vui_render(ui, frame));
    unsigned id = VUI_VIBE_OBJ_KEY_HOME_HERO;
    vui_dot_t dots[] = {
        {.x = 60 * 16, .y = 60 * 16, .radius = 6 * 16, .red = 255, .brightness = 255},
        {.x = -16, .y = 40 * 16, .radius = 6 * 16, .green = 255, .brightness = 128},
    };
    assert(vui_set_dots(ui, id, NULL, 1) == VUI_ERR_INVALID_ARG);
    assert(vui_set_dots(ui, id, dots, VUI_MAX_DOTS + 1) == VUI_ERR_INVALID_ARG);
    assert(vui_set_dots(ui, VUI_VIBE_BIND_HOME_VISIBLE, dots, 2) == VUI_ERR_INVALID_ARG);
    assert(vui_set_dots(ui, id, dots, 2) == VUI_OK);
    vui_area_t area;
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 180 && area.y == 60 && area.w == 120 && area.h == 120);
    assert(frame[120 * 480 + 240] == 0xf800);
    assert(frame[100 * 480 + 180] == 0x0400);
    assert(frame[100 * 480 + 179] == 0); /* Clipped to the node. */
    compare_full(ui, frame);
    assert(vui_set_dots(ui, id, dots, 2) == VUI_OK);
    assert(!vui_render(ui, frame));
    /* Caller memory can be reused immediately; the renderer owns a copy. */
    dots[0].x = 80 * 16;
    vui_invalidate(ui); assert(vui_render(ui, frame));
    assert(frame[120 * 480 + 240] == 0xf800);
    assert(vui_set_dots(ui, id, dots, 2) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(frame[120 * 480 + 240] == 0);
    assert(frame[120 * 480 + 260] == 0xf800);
    compare_full(ui, frame);
    assert(vui_set_dots(ui, id, NULL, 0) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(frame[120 * 480 + 260] == 0);
    assert(vui_set_dots(ui, id, NULL, 0) == VUI_OK);
    assert(!vui_render(ui, frame));
    assert(vui_set_dots(ui, id, dots, 2) == VUI_OK);
    vui_destroy(ui); /* Non-empty point storage is released as well. */
}
static void partial_and_scroll(vui_handle_t ui, uint16_t *frame)
{
    for (unsigned i = 0; i < 5; ++i) vui_tick(ui, 50);
    assert(vui_render(ui, frame));
    assert(vui_set_text(ui, VUI_VIBE_BIND_ABOUT_VERSION, "hidden change") == 0);
    assert(!vui_render(ui, frame));
    vui_tick(ui, 140);
    vui_area_t area;
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 180 && area.y == 60 && area.w == 120 && area.h == 120);
    compare_full(ui, frame);
    /* The animation's small paint clip must not remove the Settings hit. */
    vui_pointer(ui, 390, 360, true); vui_pointer(ui, 390, 360, false);
    assert(vui_render(ui, frame));
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 120) == 0);
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 0 && area.y == 96 && area.w == 480 && area.h == 352);
    compare_full(ui, frame);
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 0) == 0);
    assert(vui_render(ui, frame));
    /* All samples arrive in one render cycle, with 100 ms of actual movement.
     * Velocity must use sample time, not the consumption time of the batch. */
    vui_pointer_at(ui, 240, 350, true, 1000);
    vui_pointer_at(ui, 240, 300, true, 1100);
    vui_pointer_at(ui, 240, 300, false, 1100);
    assert(vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT) == 50);
    vui_tick(ui, 20);
    int offset = vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT);
    assert(offset >= 58 && offset <= 61);
    vui_render(ui, frame); compare_full(ui, frame);
    vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 0); vui_render(ui, frame);
    vui_pointer_at(ui, 240, 350, true, 2000);
    vui_pointer_at(ui, 240, 300, true, 2100);
    vui_pointer_at(ui, 240, 300, false, 2400);
    vui_tick(ui, 40);
    assert(vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT) == 50);
    vui_render(ui, frame); compare_full(ui, frame);
}
static void activity_progress(uint16_t *frame)
{
    vui_handle_t ui;
    assert(vui_create(&ui) == VUI_OK);
    assert(vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false) == VUI_OK);
    assert(vui_set_visible(ui, VUI_VIBE_BIND_UPDATE_VISIBLE, true) == VUI_OK);
    assert(vui_set_indeterminate(ui, VUI_VIBE_BIND_PROGRESS, true) == VUI_OK);
    assert(vui_render(ui, frame));
    uint16_t *before = malloc(480 * 480 * 2);
    assert(before); memcpy(before, frame, 480 * 480 * 2);
    vui_tick(ui, 200);
    vui_area_t area;
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 24 && area.y == 292 && area.w == 432 && area.h == 8);
    assert(memcmp(before, frame, 480 * 480 * 2));
    compare_full(ui, frame);
    assert(vui_set_indeterminate(ui, VUI_VIBE_BIND_PROGRESS, false) == VUI_OK);
    assert(vui_set_value(ui, VUI_VIBE_BIND_PROGRESS, 50) == VUI_OK);
    assert(vui_render(ui, frame));
    vui_tick(ui, 200); assert(!vui_render(ui, frame));
    free(before); vui_destroy(ui);
}
int main(void)
{
    queue_contract();
    motion_contract();
    vui_handle_t ui=NULL;
    assert(vui_create(&ui)==0);
    void *handle=vui_timer_create(ui,10,timer,&handle);
    assert(handle);
    vui_tick(ui,10);vui_tick(ui,100);assert(ticks==1);
    unsigned id=VUI_VIBE_BIND_IDEA_TITLE_PIXELS;
    void *data=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,data,1,released,data)==VUI_ERR_INVALID_ARG);
    assert(releases==0);
    assert(vui_canvas_push(ui,id,data,328*2,released,data)==0);
    void *next=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,next,328*2,released,next)==0 && releases==1);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vibe_pc_init(ui)==0);
    uint16_t *frame=malloc(480*480*2);
    dot_rendering(frame);
    activity_progress(frame);
    partial_and_scroll(ui, frame);
    unsigned random=73078;
    for(unsigned i=0;i<6000;i++) {
        random=random*1664525U+1013904223U;
        if(i%3==0)vui_pointer(ui,(random>>8)%480,(random>>18)%480,(i%9)!=0);
        vui_tick(ui,20);
        vui_render(ui,frame);
        if(i%20==0) compare_full(ui,frame);
    }
    vibe_pc_deinit(ui);
    data=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,data,328*2,released,data)==0);
    vui_destroy(ui);assert(releases==3);free(frame);
    puts("Vui retained rendering, input queue, sample timing and resource stress passed");
}
