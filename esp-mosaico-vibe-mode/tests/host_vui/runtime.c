// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_ids.h"
#include "ideas_presentation.h"
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
    vibe_home_motion_t motion = {0};
    vibe_home_motion_enter(&motion, 7000);
    vui_matrix_t first = motion.frame;
    for (unsigned t = 40; t <= 30000; t += 40) {
        vibe_home_motion_sample(&motion, 7000 + t);
        for (unsigned i = 0; i < VIBE_MOTION_PIXELS; ++i) {
            assert(motion.frame.white[i] == 0 || motion.frame.white[i] == 255);
            assert(motion.frame.accent[i] == 0 || motion.frame.accent[i] == 255);
            assert(!motion.frame.white[i] || !motion.frame.accent[i]);
        }
    }
    assert(memcmp(&first, &motion.frame, sizeof(first)));
    /* Leaving Home freezes the story. Reentering resumes the same pose. */
    vui_matrix_t paused = motion.frame;
    vibe_companion_t companion = motion.companion;
    vibe_home_motion_enter(&motion, 80000);
    assert(!memcmp(&paused, &motion.frame, sizeof(paused)));
    assert(!memcmp(&companion, &motion.companion, sizeof(companion)));
    vibe_home_motion_input(&motion, 81, 61, true, false);
    assert(motion.companion.down);
    vibe_home_motion_input(&motion, 140, 110, true, false);
    vibe_home_motion_input(&motion, 140, 110, false, true);
    assert(!motion.companion.down);
    /* A stalled or backwards clock is bounded and remains in the matrix. */
    vibe_home_motion_sample(&motion, UINT64_C(1000000000000));
    vibe_home_motion_sample(&motion, 1);
}
static void matrix_rendering(uint16_t *frame)
{
    vui_handle_t ui;
    assert(vui_create(&ui) == VUI_OK);
    assert(vui_render(ui, frame));
    unsigned id = VUI_VIBE_OBJ_KEY_HOME_HERO;
    vui_matrix_t matrix = {0};
    matrix.white[7 * 15 + 7] = 255;
    assert(vui_set_matrix(ui, VUI_VIBE_BIND_HOME_VISIBLE, &matrix) == VUI_ERR_INVALID_ARG);
    assert(vui_set_matrix(ui, id, &matrix) == VUI_OK);
    vui_area_t area;
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 280 && area.y == 64 && area.w == 172 && area.h == 172);
    uint16_t white = frame[150 * 480 + 366];
    assert(white > 0xe000);
    compare_full(ui, frame);
    assert(vui_set_matrix(ui, id, &matrix) == VUI_OK);
    assert(!vui_render(ui, frame));
    /* The renderer copies caller memory and releases it on clear/destroy. */
    matrix.white[7 * 15 + 7] = 0;
    matrix.accent[7 * 15 + 7] = 255;
    vui_invalidate(ui); assert(vui_render(ui, frame));
    assert(frame[150 * 480 + 366] == white);
    assert(vui_set_matrix(ui, id, &matrix) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(frame[150 * 480 + 366] != white);
    compare_full(ui, frame);
    assert(vui_set_matrix(ui, id, NULL) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(vui_set_matrix(ui, id, NULL) == VUI_OK);
    assert(!vui_render(ui, frame));
    assert(vui_set_matrix(ui, id, &matrix) == VUI_OK);
    vui_destroy(ui);
}
static void keyboard_preview(uint16_t *frame)
{
    vui_handle_t ui;
    assert(vui_create(&ui) == VUI_OK);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_PASSWORD_VISIBLE, true);
    assert(vui_render(ui, frame));
    uint16_t *idle = malloc(480 * 480 * 2);
    memcpy(idle, frame, 480 * 480 * 2);
    vui_pointer(ui, 35, 250, true);
    assert(vui_render(ui, frame));
    assert(memcmp(idle + 160 * 480, frame + 160 * 480, 50 * 480 * 2));
    compare_full(ui, frame);
    vui_pointer(ui, 35, 250, false);
    assert(vui_render(ui, frame));
    assert(!memcmp(idle, frame, 480 * 480 * 2));
    compare_full(ui, frame);
    vui_pointer(ui, 35, 250, true); vui_render(ui, frame);
    vui_pointer(ui, 140, 250, true); vui_render(ui, frame);
    compare_full(ui, frame);
    vui_pointer(ui, 140, 250, false); vui_render(ui, frame);
    assert(!memcmp(idle, frame, 480 * 480 * 2));
    free(idle); vui_destroy(ui);
}
static void partial_and_scroll(vui_handle_t ui, uint16_t *frame)
{
    for (unsigned i = 0; i < 5; ++i) vui_tick(ui, 50);
    assert(vui_render(ui, frame));
    assert(vui_set_text(ui, VUI_VIBE_BIND_ABOUT_VERSION, "hidden change") == 0);
    assert(!vui_render(ui, frame));
    vui_area_t area;
    bool changed = false;
    for (unsigned i = 0; i < 60 && !changed; ++i) {
        vui_tick(ui, 40); changed = vui_render_changed(ui, frame, &area);
    }
    assert(changed);
    assert(area.x == 280 && area.y == 64 && area.w == 172 && area.h == 172);
    compare_full(ui, frame);
    /* The animation's small paint clip must not remove the Settings hit. */
    vui_pointer(ui, 390, 360, true); vui_pointer(ui, 390, 360, false);
    assert(vui_render(ui, frame));
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 120) == 0);
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 0 && area.y == 96 && area.w == 480 && area.h == 368);
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
    assert(area.x == 40 && area.y == 286 && area.w == 400 && area.h == 12);
    assert(memcmp(before, frame, 480 * 480 * 2));
    compare_full(ui, frame);
    assert(vui_set_indeterminate(ui, VUI_VIBE_BIND_PROGRESS, false) == VUI_OK);
    assert(vui_set_value(ui, VUI_VIBE_BIND_PROGRESS, 50) == VUI_OK);
    assert(vui_render(ui, frame));
    vui_tick(ui, 200); assert(!vui_render(ui, frame));
    free(before); vui_destroy(ui);
}
static void title_marquee(uint16_t *frame)
{
    vui_handle_t ui;
    assert(vui_create(&ui) == VUI_OK);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_CONFIRM_VISIBLE, true);
    vui_set_visible(ui, VUI_VIBE_BIND_RELEASE_PIXELS, true);
    uint16_t *pixels = malloc(640 * 28 * 2);
    assert(pixels);
    for (unsigned y = 0; y < 28; ++y)
        for (unsigned x = 0; x < 640; ++x)
            pixels[y*640+x] = x % 32 < 20 ? (x < 320 ? 0xf800 : 0x001f) : 0;
    size_t size = 0;
    void *qoi = ideas_title_qoi(pixels, 640, 28, &size);
    assert(qoi && vui_set_image_owned(ui, VUI_VIBE_BIND_RELEASE_PIXELS, qoi, size) == VUI_OK);
    assert(vui_render(ui, frame));
    uint16_t *first = malloc(480 * 480 * 2);
    assert(first); memcpy(first, frame, 480 * 480 * 2);
    /* Exact native geometry: the 20x28 first glyph is not enlarged or squashed. */
    for (unsigned y = 35; y < 63; ++y)
        for (unsigned x = 88; x < 108; ++x) assert(frame[y*480+x] == 0xf800);
    assert(frame[34*480+90] != 0xf800 && frame[63*480+90] != 0xf800);
    assert(frame[40*480+108] != 0xf800);
    vui_tick(ui, 1000);
    assert(!vui_render(ui, frame));
    vui_tick(ui, 1200);
    vui_area_t damage;
    assert(vui_render_changed(ui, frame, &damage));
    assert(damage.x == 88 && damage.y == 29 && damage.w == 368 && damage.h == 40);
    for (unsigned y = 0; y < 480; ++y)
        for (unsigned x = 0; x < 480; ++x)
            if (x < 88 || x >= 456 || y < 29 || y >= 69)
                assert(frame[y*480+x] == first[y*480+x]);
    assert(frame[40*480+88] != first[40*480+88]);
    compare_full(ui, frame);
    vui_tick(ui, 8815); /* End reached, then pause for reading. */
    assert(vui_render(ui, frame));
    vui_tick(ui, 800);
    assert(!vui_render(ui, frame));
    vui_set_visible(ui, VUI_VIBE_BIND_CONFIRM_VISIBLE, false);
    vui_tick(ui, 20);
    vui_set_visible(ui, VUI_VIBE_BIND_CONFIRM_VISIBLE, true);
    assert(vui_render(ui, frame));
    assert(!memcmp(first, frame, 480 * 480 * 2));
    for (unsigned i = 0; i < 80*28; ++i) pixels[i] = 0xf800;
    qoi = ideas_title_qoi(pixels, 80, 28, &size);
    assert(qoi && vui_set_image_owned(ui, VUI_VIBE_BIND_RELEASE_PIXELS, qoi, size) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(frame[40*480+167] == 0xf800 && frame[40*480+168] != 0xf800);
    vui_tick(ui, 5000); assert(!vui_render(ui, frame));
    vui_set_visible(ui, VUI_VIBE_BIND_RELEASE_PIXELS, false);
    vui_set_text(ui, VUI_VIBE_BIND_RELEASE, "A long title that scrolls at its original font size without ellipsis");
    assert(vui_render(ui, frame));
    memcpy(first, frame, 480*480*2);
    vui_tick(ui, 3000); assert(vui_render(ui, frame));
    assert(memcmp(first, frame, 480*480*2));
    compare_full(ui, frame);
    vui_set_text(ui, VUI_VIBE_BIND_RELEASE, "Short");
    assert(vui_render(ui, frame));
    vui_tick(ui, 5000); assert(!vui_render(ui, frame));
    free(first); free(pixels); vui_destroy(ui);
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
    unsigned id=VUI_VIBE_BIND_IDEA_BODY_PIXELS;
    void *data=malloc(416*176*2);
    assert(vui_canvas_push(ui,id,data,1,released,data)==VUI_ERR_INVALID_ARG);
    assert(releases==0);
    assert(vui_canvas_push(ui,id,data,416*2,released,data)==0);
    void *next=malloc(416*176*2);
    assert(vui_canvas_push(ui,id,next,416*2,released,next)==0 && releases==1);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vibe_pc_init(ui)==0);
    uint16_t *frame=malloc(480*480*2);
    title_marquee(frame);
    matrix_rendering(frame);
    keyboard_preview(frame);
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
    data=malloc(416*176*2);
    assert(vui_canvas_push(ui,id,data,416*2,released,data)==0);
    vui_destroy(ui);assert(releases==3);free(frame);
    puts("Vui retained rendering, input queue, sample timing and resource stress passed");
}
