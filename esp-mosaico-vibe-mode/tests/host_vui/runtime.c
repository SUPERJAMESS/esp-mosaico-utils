// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_ids.h"
#include "ideas_presentation.h"
#include "vibe_input_queue.h"
#include "vibe_home_motion.h"
#include "vibe_ui_state.h"
#include "vui_transition.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
extern int vibe_pc_init(vui_handle_t ui);
extern void vibe_pc_deinit(vui_handle_t ui);
static unsigned releases, ticks;
static void released(void *data) { releases++; free(data); }
static void timer(vui_handle_t ui, void *ctx) { ticks++; assert(vui_timer_delete(ui,*(void **)ctx)==0); }
typedef struct { vibe_snapshot_t value; unsigned resets, deletes, formats, key; } clock_model_t;
static void clock_snapshot(void *ctx, vibe_snapshot_t *out) { *out = ((clock_model_t *)ctx)->value; }
static int clock_command(void *ctx, vibe_command_t command, const char *a, const char *b) {
    if (command == VIBE_DELETE_IDEA) ++((clock_model_t *)ctx)->deletes;
    if (command == VIBE_FORMAT_NAND) ++((clock_model_t *)ctx)->formats;
    return 0;
}
static int clock_setting(void *ctx, vibe_settings_command_t command, const char *a, const char *b) {
    if (command == VIBE_SETTINGS_RESET) ++((clock_model_t *)ctx)->resets;
    return 0;
}
static unsigned clock_key(void *ctx) {
    clock_model_t *model = ctx; unsigned key = model->key; model->key = 0; return key;
}
static void advance_clock(vui_handle_t ui, uint16_t *frame, unsigned ms, unsigned step) {
    while (ms) {
        unsigned elapsed = ms < step ? ms : step;
        vui_tick(ui, elapsed); vui_render(ui, frame); ms -= elapsed;
    }
}
static void controller_time_contract(void) {
    const unsigned steps[] = {16,33,40,100};
    uint16_t *frame = malloc(480*480*2);
    uint16_t *hint = malloc(480*40*2);
    assert(frame && hint);
    for (unsigned n=0;n<sizeof(steps)/sizeof(*steps);++n) {
        const unsigned step=steps[n];
        clock_model_t model={.value={.settings={.language=1,.screen_off_seconds=60},
            .storage_total=1000000,.nand_generation=1,.bundle_count=1}};
        strcpy(model.value.bundles[0].code,"123456");
        const vibe_services_t services={.ctx=&model,.snapshot=clock_snapshot,
            .command=clock_command,.setting=clock_setting};
        vui_handle_t ui=NULL; vibe_ui_t *state=NULL;
        assert(!vui_create(&ui) && !vibe_ui_create(ui,&services,&state));
        vibe_show(state,VIBE_RESET); vui_render(ui,frame);
        advance_clock(ui,frame,200,step);
        vui_pointer(ui,240,400,true);
        advance_clock(ui,frame,2900,step); assert(!model.resets);
        advance_clock(ui,frame,100,step); assert(model.resets==1);
        advance_clock(ui,frame,1000,step); assert(model.resets==1);
        vui_pointer(ui,240,400,false);
        vibe_show(state,VIBE_DELETE_CONFIRM);
        strcpy(state->delete_code,"123456");
        advance_clock(ui,frame,200,step); vui_render(ui,frame);
        memcpy(hint,frame+20*480,480*40*2);
        vui_set_text(ui,VUI_VIBE_BIND_DELETE_CONFIRM_TITLE,"Delete Idea"); vui_render(ui,frame);
        assert(memcmp(hint,frame+20*480,480*40*2)); /* Dynamic header binding must be visible. */
        memcpy(hint,frame+276*480,480*40*2);
        vui_pointer(ui,240,400,true);
        advance_clock(ui,frame,1100,step);
        vui_pointer(ui,240,400,false); vui_render(ui,frame);
        assert(!state->hold_started_ms && !state->undo_until && !model.deletes);
        assert(!memcmp(hint,frame+276*480,480*40*2)); /* Interrupted hold resets its hint. */
        vibe_show(state,VIBE_HOME); vibe_show(state,VIBE_DELETE_CONFIRM);
        advance_clock(ui,frame,200,step); vui_render(ui,frame);
        assert(!memcmp(hint,frame+276*480,480*40*2));
        vui_pointer(ui,240,400,true);
        advance_clock(ui,frame,3000,step);
        assert(state->undo_until && !model.deletes);
        vui_pointer(ui,240,400,false);
        advance_clock(ui,frame,3900,step); assert(!model.deletes);
        advance_clock(ui,frame,100,step); assert(model.deletes==1 && !state->undo_until);
        vibe_ui_deinit(state); vui_destroy(ui);

        assert(!vui_create(&ui) && !vibe_ui_create(ui,&services,&state));
        advance_clock(ui,frame,59900,step); assert(state->page!=VIBE_SLEEP);
        advance_clock(ui,frame,100,step); assert(state->page==VIBE_SLEEP);
        vibe_ui_deinit(state); vui_destroy(ui);
    }
    free(hint); free(frame);
}
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
    assert(vibe_input_push(&queue, event) && queue.count <= VIBE_INPUT_CAPACITY);
    assert(vibe_input_pop(&queue, &event) && event.kind == VIBE_INPUT_DOWN && event.x == 420);
    unsigned checkpoints = 0;
    uint64_t last_time = 1000;
    while (vibe_input_pop(&queue, &event) && event.kind == VIBE_INPUT_MOVE) {
        assert(event.time_ms > last_time);
        last_time = event.time_ms;
        ++checkpoints;
    }
    assert(checkpoints >= 10 && last_time == 1360);
    assert(event.kind == VIBE_INPUT_UP && event.x == 60);
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
    assert(vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT) == 38);
    vui_tick(ui, 20);
    int offset = vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT);
    assert(offset >= 46 && offset <= 48);
    vui_render(ui, frame); compare_full(ui, frame);
    vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 0); vui_render(ui, frame);
    vui_pointer_at(ui, 240, 350, true, 2000);
    vui_pointer_at(ui, 240, 300, true, 2100);
    vui_pointer_at(ui, 240, 300, false, 2400);
    vui_tick(ui, 40);
    assert(vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT) == 38);
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
    assert(area.x == 40 && area.y == 316 && area.w == 400 && area.h == 12);
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
static unsigned row_bindings[16];
static vui_err_t bind_visible_row(vui_handle_t ui, vui_row_t row, uint32_t index, void *ctx)
{
    (void)ctx; assert(index < 16); ++row_bindings[index];
    char label[24]; snprintf(label, sizeof(label), "Visible row %u", (unsigned)index);
    assert(vui_vibe_bundles_row_row_set_bundles_title_text(ui, row, label) == VUI_OK);
    uint16_t pixels[4] = {0xf800, 0x07e0, 0x001f, 0xffff}; size_t size;
    void *image = ideas_title_qoi(pixels, 2, 2, &size); assert(image);
    assert(vui_row_set_image_owned(ui, row, VUI_VIBE_BIND_BUNDLES_PIXELS, image, size) == VUI_OK);
    return VUI_OK;
}
static void list_binding_contract(uint16_t *frame)
{
    vui_handle_t ui; assert(vui_create(&ui) == VUI_OK);
    vui_list_t list = vui_list_bind_component(ui, VUI_VIBE_OBJ_KEY_BUNDLES, bind_visible_row, NULL);
    assert(list && vui_list_set_total(ui, list, 16) == VUI_OK);
    assert(vui_list_refresh(ui, list) == VUI_OK);
    assert(!row_bindings[0] && !row_bindings[15]);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_NAND_VISIBLE, true);
    vui_set_visible(ui, VUI_VIBE_BIND_BUNDLES_VISIBLE, true);
    assert(vui_render(ui, frame));
    assert(row_bindings[0] == 1 && !row_bindings[15]);
    compare_full(ui, frame); assert(row_bindings[0] == 1);
    assert(vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_BUNDLES, 100000) == VUI_OK);
    assert(vui_render(ui, frame));
    assert(row_bindings[0] == 1 && row_bindings[15] == 1);
    compare_full(ui, frame);
    vui_set_visible(ui, VUI_VIBE_BIND_NAND_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, true);
    assert(vui_list_refresh(ui, list) == VUI_OK);
    assert(vui_render(ui, frame) && row_bindings[15] == 1);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_NAND_VISIBLE, true);
    assert(vui_render(ui, frame) && row_bindings[15] == 2);
    assert(vui_list_set_total(ui, list, 0) == VUI_OK);
    vui_render(ui, frame); vui_destroy(ui);
}
static void list_marquee_damage(void)
{
    clock_model_t model={.value={.settings={.language=1},.nand_generation=1,
        .storage_total=1000000,.bundle_count=1}};
    strcpy(model.value.bundles[0].code,"123456");
    strcpy(model.value.bundles[0].release,"A long Idea title that scrolls while every other list pixel stays still");
    const vibe_services_t services={.ctx=&model,.snapshot=clock_snapshot,
        .command=clock_command,.setting=clock_setting};
    vui_handle_t ui=NULL; vibe_ui_t *state=NULL;
    assert(!vui_create(&ui) && !vibe_ui_create(ui,&services,&state));
    uint16_t *frame=malloc(480*480*2); assert(frame);
    vibe_show(state,VIBE_NAND);
    advance_clock(ui,frame,2000,10);
    unsigned changed=0;
    for(unsigned i=0;i<100;i++) {
        vui_tick(ui,40);
        vui_area_t area;
        if(vui_render_changed(ui,frame,&area)) {
            assert(area.x==100 && area.y==156 && area.w==312 && area.h==34);
            ++changed;
            compare_full(ui,frame);
        }
    }
    assert(changed>10);
    vibe_ui_deinit(state); vui_destroy(ui); free(frame);
}
static vui_handle_t scrolling_ui(uint16_t *frame, int offset)
{
    vui_handle_t ui = NULL;
    assert(vui_create(&ui) == VUI_OK);
    vui_set_visible(ui, VUI_VIBE_BIND_HOME_VISIBLE, false);
    vui_set_visible(ui, VUI_VIBE_BIND_SETTINGS_VISIBLE, true);
    vui_scroll_to(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, offset);
    vui_render(ui, frame);
    return ui;
}
static int scroll_at(vui_handle_t ui) { return vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT); }
static void coast(vui_handle_t ui, unsigned step)
{
    for (unsigned elapsed = 0; elapsed < 2000; elapsed += step) vui_tick(ui, step);
}
static void gesture_contract(void)
{
    uint16_t *frame = malloc(480 * 480 * 2);
    assert(frame);
    vui_handle_t ui = scrolling_ui(frame, 100);
    vui_pointer_at(ui, 240, 350, true, 0);
    vui_pointer_at(ui, 300, 345, true, 50);
    vui_pointer_at(ui, 300, 345, false, 50);
    coast(ui, 16); assert(scroll_at(ui) == 100); /* Horizontal jitter is not a vertical drag. */
    vui_destroy(ui);
    for (unsigned fine = 0; fine < 2; ++fine) {
        ui = scrolling_ui(frame, 100);
        vui_pointer_at(ui, 240, 400, true, 0);
        if (fine) for (unsigned i = 1; i <= 4; ++i) vui_pointer_at(ui, 240, 400 - 3 * i, true, i * 10);
        vui_pointer_at(ui, 240, 387, true, 50);
        assert(scroll_at(ui) == 101); /* Only the distance past the 12 px slop is visible. */
        vui_pointer_at(ui, 240, 387, false, 50);
        vui_destroy(ui);
    }
    ui = scrolling_ui(frame, 100);
    vui_pointer_at(ui, 240, 400, true, 0);
    for (unsigned i = 1; i <= 4; ++i) vui_pointer_at(ui, 240, 400 - 5 * i, true, i * 50);
    vui_pointer_at(ui, 240, 380, false, 200);
    int stopped = scroll_at(ui); assert(stopped == 108);
    coast(ui, 16); assert(scroll_at(ui) == stopped); /* Slow dragging stops on release. */
    vui_destroy(ui);
    for (unsigned pause = 0; pause < 2; ++pause) {
        ui = scrolling_ui(frame, 100);
        vui_pointer_at(ui, 240, 400, true, 0);
        for (unsigned i = 1; i <= 4; ++i) vui_pointer_at(ui, 240, 400 - 20 * i, true, i * 20);
        vui_pointer_at(ui, 240, 321, true, 100); /* A 1 px reverse tail must not reverse the coast. */
        vui_pointer_at(ui, 240, 321, false, pause ? 300 : 100);
        stopped = scroll_at(ui);
        coast(ui, 16);
        assert(pause ? scroll_at(ui) == stopped : scroll_at(ui) > stopped);
        vui_destroy(ui);
    }
    /* Cadence does not change the same trajectory's release distance. */
    const unsigned steps[] = {1, 5, 10, 16, 33, 40, 100};
    int shortest = 10000, longest = 0;
    for (unsigned i = 0; i < sizeof(steps) / sizeof(*steps); ++i) {
        ui = scrolling_ui(frame, 0);
        vui_pointer_at(ui, 240, 400, true, 0);
        vui_pointer_at(ui, 240, 380, true, 40);
        vui_pointer_at(ui, 240, 360, true, 80);
        vui_pointer_at(ui, 240, 360, false, 80);
        coast(ui, steps[i]);
        int offset = scroll_at(ui);
        if (offset < shortest) shortest = offset;
        if (offset > longest) longest = offset;
        vui_destroy(ui);
    }
    assert(longest - shortest <= 2);
    free(frame);
}
static void target_identity_contract(void)
{
    const vibe_page_t pages[] = {VIBE_NAND, VIBE_WIFI, VIBE_GRID, VIBE_SHARED};
    const int x[] = {150, 150, 92, 150}, y[] = {170, 250, 150, 130};
    const vibe_page_t accepted[] = {VIBE_CONFIRM, VIBE_PASSWORD, VIBE_OPEN_CONFIRM, VIBE_SHARED_DETAIL};
    uint16_t *frame = malloc(480 * 480 * 2);
    assert(frame);
    for (unsigned page = 0; page < 4; ++page) for (unsigned replace = 0; replace < 2; ++replace) {
        clock_model_t model = {.value = {.settings = {.language=1, .open_confirm=true, .shared_count=2},
            .storage_total=1000000, .nand_generation=1, .bundle_count=2, .scan_generation=1, .ap_count=2}};
        strcpy(model.value.bundles[0].code, "111111"); strcpy(model.value.bundles[0].release, "First");
        strcpy(model.value.bundles[1].code, "222222"); strcpy(model.value.bundles[1].release, "Second");
        strcpy(model.value.aps[0].ssid, "First network"); strcpy(model.value.aps[1].ssid, "Second network");
        strcpy(model.value.settings.shared[0].key, "first"); strcpy(model.value.settings.shared[1].key, "second");
        const vibe_services_t services = {.ctx=&model, .snapshot=clock_snapshot, .command=clock_command};
        vui_handle_t ui = NULL; vibe_ui_t *state = NULL;
        assert(!vui_create(&ui) && !vibe_ui_create(ui, &services, &state));
        vibe_show(state, pages[page]); vui_tick(ui, 200); vui_render(ui, frame);
        vui_pointer(ui, x[page], y[page], true);
        if (replace) {
            vibe_bundle_t bundle = model.value.bundles[0];
            model.value.bundles[0] = model.value.bundles[1]; model.value.bundles[1] = bundle;
            vibe_ap_t ap = model.value.aps[0]; model.value.aps[0] = model.value.aps[1]; model.value.aps[1] = ap;
            vibe_shared_entry_t entry = model.value.settings.shared[0];
            model.value.settings.shared[0] = model.value.settings.shared[1]; model.value.settings.shared[1] = entry;
        } else {
            strcpy(model.value.bundles[0].release, "Changed title, same Idea");
            model.value.aps[0].rssi = -42;
            strcpy(model.value.settings.shared[0].value, "Changed value, same key");
        }
        ++model.value.nand_generation; ++model.value.scan_generation; ++model.value.settings.revision;
        vibe_ui_poll(state); vui_render(ui, frame);
        vui_pointer(ui, x[page], y[page], false);
        assert(state->page == (replace ? pages[page] : accepted[page]));
        vibe_ui_deinit(state); vui_destroy(ui);
    }
    free(frame);
}
static void pager_create(clock_model_t *model, vui_handle_t *ui, vibe_ui_t **state, uint16_t *frame)
{
    *model = (clock_model_t){.value={.settings={.language=1,.screen_off_seconds=180,.open_confirm=true},
        .storage_total=1000000,.nand_generation=1,.bundle_count=16}};
    for (unsigned i = 0; i < 16; ++i) {
        snprintf(model->value.bundles[i].code, sizeof(model->value.bundles[i].code), "%06u", i + 1);
        snprintf(model->value.bundles[i].release, sizeof(model->value.bundles[i].release), "Idea %u", i + 1);
    }
    const vibe_services_t services = {.ctx=model,.snapshot=clock_snapshot,.command=clock_command,
        .setting=clock_setting,.key=clock_key};
    assert(!vui_create(ui) && !vibe_ui_create(*ui, &services, state));
    advance_clock(*ui, frame, 200, 16);
}
static void pager_swipe(vui_handle_t ui, uint16_t *frame, int from, int to)
{
    uint64_t now = vui_now_ms(ui);
    vui_pointer_at(ui, from, 250, true, now);
    vui_pointer_at(ui, to, 250, true, now + 100);
    vui_render(ui, frame);
    vui_pointer_at(ui, to, 250, false, now + 110);
    advance_clock(ui, frame, 240, 16);
}
static void pager_render_contract(void)
{
    uint16_t *frame = malloc(480 * 480 * 2), *home = malloc(480 * 480 * 2);
    uint16_t *grid = malloc(480 * 480 * 2), *other = malloc(480 * 480 * 2);
    assert(frame && home && grid && other);
    clock_model_t model; vui_handle_t ui = NULL; vibe_ui_t *state = NULL;
    pager_create(&model, &ui, &state, frame);
    vibe_show(state, VIBE_GRID); vibe_product_poll(state); vui_render(ui, frame);
    memcpy(grid, frame, 480 * 480 * 2);
    vibe_show(state, VIBE_HOME); advance_clock(ui, frame, 200, 16);
    memcpy(home, frame, 480 * 480 * 2);
    vui_pointer_at(ui, 420, 250, true, 1000);
    vui_pointer_at(ui, 300, 250, true, 1120);
    assert(state->page == VIBE_HOME && state->pager.active && state->pager.offset == -108);
    vui_area_t area;
    assert(vui_render_changed(ui, frame, &area));
    assert(area.x == 0 && area.y == 56 && area.w == 480 && area.h == 392);
    assert(!memcmp(home, frame, 56 * 480 * 2));
    assert(!memcmp(home + 448 * 480, frame + 448 * 480, 32 * 480 * 2));
    for (unsigned y = 56; y < 448; ++y) {
        assert(!memcmp(frame + y * 480, home + y * 480 + 108, 372 * 2));
        assert(!memcmp(frame + y * 480 + 372, grid + y * 480, 108 * 2));
    }
    /* Replacing the retained surface must not read the old surface or lose
     * the frozen header/footer. Full invalidation preserves the same blend. */
    assert(vui_render(ui, other)); assert(!memcmp(frame, other, 480 * 480 * 2));
    assert(vui_render(ui, frame)); compare_full(ui, frame);
    assert(vui_transition_begin(ui, (vui_area_t){0,56,480,392}) == VUI_ERR_INVALID_STATE);
    vui_pointer_at(ui, 300, 250, false, 1500); /* Held short drag cancels. */
    assert(state->pager.settling && state->page == VIBE_HOME);
    advance_clock(ui, frame, 240, 33);
    assert(!state->pager.active && state->page == VIBE_HOME);
    /* The device ticks timers before draining input and drawing. A tap in
     * that gap must not use hit regions left over from a translated page. */
    vui_pointer_at(ui, 420, 250, true, 1600);
    vui_pointer_at(ui, 300, 250, true, 1620);
    vui_render(ui, frame);
    vui_pointer_at(ui, 300, 250, false, 1630);
    vui_tick(ui, 240);
    assert(!state->pager.active && state->page == VIBE_GRID);
    vui_pointer(ui, 420, 150, true); vui_pointer(ui, 420, 150, false);
    assert(state->page == VIBE_GRID);
    vui_render(ui, frame);
    vui_tick(ui, 160);
    vui_pointer(ui, 420, 150, true); vui_pointer(ui, 420, 150, false);
    assert(state->page == VIBE_OPEN_CONFIRM && !strcmp(state->selected_bundle.code, "000003"));
    vibe_show(state, VIBE_HOME); advance_clock(ui, frame, 200, 16);
    /* Frozen transitions also retire safely during deinit, without an UP. */
    vui_pointer_at(ui, 420, 250, true, 2000);
    vui_pointer_at(ui, 200, 250, true, 2100);
    assert(state->pager.active);
    vibe_ui_deinit(state); vui_destroy(ui);
    free(other); free(grid); free(home); free(frame);
}
static void pager_input_contract(void)
{
    typedef struct { int x[3]; unsigned ms[3]; unsigned moves; bool commit; } trace_t;
    const trace_t traces[] = {
        {{360}, {500}, 1, false},                   /* Deliberate short, slow drag. */
        {{400,380}, {20,40}, 2, true},              /* Short fast flick. */
        {{280}, {400}, 1, true},                    /* Distance commits without speed. */
        {{200,240,280}, {100,140,180}, 3, false},    /* Deliberate reverse cancels. */
        {{240,241}, {80,100}, 2, true},             /* One pixel of noise does not cancel. */
    };
    uint16_t *frame = malloc(480 * 480 * 2); assert(frame);
    for (unsigned i = 0; i < sizeof(traces)/sizeof(*traces); ++i) {
        clock_model_t model; vui_handle_t ui = NULL; vibe_ui_t *state = NULL;
        pager_create(&model, &ui, &state, frame);
        vui_pointer_at(ui,420,250,true,1000);
        for (unsigned j = 0; j < traces[i].moves; ++j)
            vui_pointer_at(ui,traces[i].x[j],250,true,1000+traces[i].ms[j]);
        unsigned last = traces[i].moves-1;
        vui_pointer_at(ui,traces[i].x[last],250,false,1000+traces[i].ms[last]+10);
        advance_clock(ui,frame,240,16);
        assert(state->page == (traces[i].commit ? VIBE_GRID : VIBE_HOME));
        assert(!state->pager.active && !model.resets && !model.deletes);
        vibe_ui_deinit(state); vui_destroy(ui);
    }
    clock_model_t model; vui_handle_t ui = NULL; vibe_ui_t *state = NULL;
    pager_create(&model, &ui, &state, frame);
    vui_pointer_at(ui,420,250,true,1000);
    vui_pointer_at(ui,400,290,true,1040);
    vui_pointer_at(ui,220,300,true,1120);
    vui_pointer_at(ui,220,300,false,1130);
    advance_clock(ui,frame,240,16);
    assert(state->page == VIBE_HOME && !state->pager.active); /* A vertical claim cannot become paging. */
    vui_pointer_at(ui,420,32,true,1200); vui_pointer_at(ui,60,32,true,1300);
    vui_pointer_at(ui,60,32,false,1310); advance_clock(ui,frame,240,16);
    assert(state->page == VIBE_HOME); /* Header gestures do not navigate. */
    pager_swipe(ui,frame,420,60); assert(state->page == VIBE_GRID && state->grid_page == 0);
    pager_swipe(ui,frame,420,60); assert(state->grid_page == 1);
    vui_pointer(ui,92,150,true); vui_pointer(ui,92,150,false);
    assert(state->page == VIBE_OPEN_CONFIRM && !strcmp(state->selected_bundle.code,"000007"));
    vibe_show(state,VIBE_GRID); advance_clock(ui,frame,200,16);
    pager_swipe(ui,frame,420,60); assert(state->grid_page == 2);
    pager_swipe(ui,frame,420,60); assert(state->page == VIBE_GRID && state->grid_page == 2);
    pager_swipe(ui,frame,60,420); assert(state->grid_page == 1);
    pager_swipe(ui,frame,60,420); assert(state->grid_page == 0);
    pager_swipe(ui,frame,60,420); assert(state->page == VIBE_HOME);
    pager_swipe(ui,frame,60,420); assert(state->page == VIBE_HOME);
    /* Regrab a settling page and pull it back without opening its icons. */
    vui_pointer_at(ui,420,250,true,3000); vui_pointer_at(ui,200,250,true,3100);
    vui_pointer_at(ui,200,250,false,3110); advance_clock(ui,frame,32,16);
    assert(state->pager.settling);
    int before = state->pager.offset;
    vui_pointer_at(ui,240,250,true,3200);
    assert(!state->pager.settling && state->pager.offset == before);
    vui_pointer_at(ui,440,250,true,3300); vui_pointer_at(ui,440,250,false,3310);
    advance_clock(ui,frame,240,16);
    assert(state->page == VIBE_HOME && !state->pager.active);
    /* A changed library, navigation or hardware key cancels the frozen page. */
    vui_pointer_at(ui,420,250,true,4000); vui_pointer_at(ui,200,250,true,4100);
    ++model.value.nand_generation; vibe_ui_poll(state); advance_clock(ui,frame,60,16);
    assert(state->pager.active); /* Capacity/badge refresh preserves page identity. */
    vibe_bundle_t changed = model.value.bundles[0];
    model.value.bundles[0] = model.value.bundles[1]; model.value.bundles[1] = changed;
    ++model.value.nand_generation; vibe_ui_poll(state); advance_clock(ui,frame,60,16);
    assert(!state->pager.active && state->page == VIBE_HOME);
    vui_pointer_at(ui,200,250,false,4110);
    vui_pointer_at(ui,420,250,true,4200); vui_pointer_at(ui,200,250,true,4300);
    model.key = 3; advance_clock(ui,frame,60,16);
    assert(!state->pager.active && state->page == VIBE_BOOT_TIP);
    vui_pointer_at(ui,200,250,false,4310);
    model.key = 4; advance_clock(ui,frame,60,16); assert(state->page == VIBE_HOME);
    vui_pointer_at(ui,420,250,true,4400); vui_pointer_at(ui,200,250,true,4500);
    model.value.nand_error = 0x101; model.value.nand_needs_format = true;
    vibe_ui_poll(state);
    assert(state->page == VIBE_FORMAT_NAND_CONFIRM && !state->pager.active);
    vui_pointer_at(ui,400,420,false,4510); /* Old drag cannot accept a newly shown destructive action. */
    assert(state->page == VIBE_FORMAT_NAND_CONFIRM && !model.formats);
    model.value.nand_error = 0; model.value.nand_needs_format = false;
    vibe_show(state,VIBE_HOME); advance_clock(ui,frame,200,16);
    /* Timer exhaustion falls back to committing the release, never leaving
     * a translated page or an unresponsive input owner. */
    void *reserved[6];
    for (unsigned i = 0; i < 6; ++i) {
        reserved[i] = vui_timer_create(ui,100000,timer,&reserved[i]); assert(reserved[i]);
    }
    vui_pointer_at(ui,420,250,true,5000); vui_pointer_at(ui,60,250,true,5100);
    vui_pointer_at(ui,60,250,false,5110);
    assert(state->page == VIBE_GRID && !state->pager.active);
    vibe_ui_deinit(state); vui_destroy(ui); free(frame);
}
int main(void)
{
    pager_render_contract();
    pager_input_contract();
    gesture_contract();
    target_identity_contract();
    list_marquee_damage();
    controller_time_contract();
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
    list_binding_contract(frame);
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
