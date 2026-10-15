// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Single-render-task API. Platform adapters serialize input, timers and draw. */
typedef struct vui *vui_handle_t;
typedef int vui_err_t;
typedef unsigned vui_list_t;
typedef struct vui_row *vui_row_t;
enum {
    VUI_OK = 0,
    VUI_ERR_NO_MEM = 0x101,
    VUI_ERR_INVALID_ARG = 0x102,
    VUI_ERR_INVALID_STATE = 0x103,
    VUI_EVENT_CALL = 1,
    VUI_LIST_NONE = 0
};
typedef struct {
    unsigned type;
    uint16_t action_id;
    uint32_t arg, item;
    vui_list_t list;
} vui_event_t;
typedef vui_err_t (*vui_row_cb_t)(vui_handle_t, vui_row_t, uint32_t, void *);
typedef void (*vui_event_cb_t)(vui_handle_t, const vui_event_t *, void *);
typedef void (*vui_pointer_cb_t)(vui_handle_t, int32_t, int32_t, bool, void *);
typedef void (*vui_timer_cb_t)(vui_handle_t, void *);
vui_err_t vui_create(vui_handle_t *out);
void vui_destroy(vui_handle_t ui);
void vui_tick(vui_handle_t ui, uint32_t elapsed_ms);
uint64_t vui_now_ms(vui_handle_t ui);
/* Procedural circles, in node-local 1/16 pixel units. RGB and brightness are
 * independent 0..255 channels. The renderer copies the points and reuses its
 * allocation across frames; NULL/0 clears them. No bitmap frames are retained. */
typedef struct {
    int16_t x, y;
    uint16_t radius;
    uint8_t red, green, blue, brightness;
} vui_dot_t;
#define VUI_MAX_DOTS 64
vui_err_t vui_set_dots(vui_handle_t ui, uint16_t id, const vui_dot_t *dots, size_t count);
/* pixels is a retained 480x480 RGB565 surface. A new surface receives a full
 * repaint; subsequent calls preserve pixels outside the returned damage. */
typedef struct { int x, y, w, h; } vui_area_t;
bool vui_render(vui_handle_t ui, uint16_t *pixels);
bool vui_render_changed(vui_handle_t ui, uint16_t *pixels, vui_area_t *changed);
void vui_invalidate(vui_handle_t ui);
/* Clipped scroll containers share visual and input coordinates. */
vui_err_t vui_scroll_to(vui_handle_t ui, uint16_t id, int offset);
int vui_scroll_offset(vui_handle_t ui, uint16_t id);
void vui_pointer(vui_handle_t ui, int x, int y, bool pressed);
/* Monotonic sample time, independent of when a queued event is consumed. */
void vui_pointer_at(vui_handle_t ui, int x, int y, bool pressed, uint64_t time_ms);
vui_err_t vui_set_text(vui_handle_t ui, uint16_t id, const char *text);
vui_err_t vui_set_visible(vui_handle_t ui, uint16_t id, bool visible);
vui_err_t vui_component_set_visible(vui_handle_t ui, uint16_t id, bool visible);
vui_err_t vui_set_value(vui_handle_t ui, uint16_t id, unsigned value);
/* Animate an activity segment when the backend has no measurable total. */
vui_err_t vui_set_indeterminate(vui_handle_t ui, uint16_t id, bool active);
/* Ownership transfers only on success. PNG and local title-cache inputs are
 * validated before replacing the previous resource; NULL/0 clears it.
 * Canvas release runs exactly once. */
vui_err_t vui_set_image_owned(vui_handle_t ui, uint16_t id, void *png, size_t size);
vui_err_t vui_canvas_push(vui_handle_t ui, uint16_t id, void *pixels, size_t stride,
                          void (*release)(void *), void *ctx);
vui_err_t vui_canvas_stop(vui_handle_t ui, uint16_t id);
vui_list_t vui_list_bind_component(vui_handle_t ui, uint16_t id, vui_row_cb_t callback, void *ctx);
vui_err_t vui_list_set_total(vui_handle_t ui, vui_list_t list, unsigned count);
vui_err_t vui_list_refresh(vui_handle_t ui, vui_list_t list);
vui_err_t vui_row_set_text(vui_handle_t ui, vui_row_t row, uint16_t id, const char *text);
vui_err_t vui_row_set_color(vui_handle_t ui, vui_row_t row, uint16_t id, uint16_t color);
vui_err_t vui_row_set_image_owned(vui_handle_t ui, vui_row_t row, uint16_t id, void *png,
                                  size_t size);
vui_err_t vui_on_event(vui_handle_t ui, vui_event_cb_t callback, void *ctx);
vui_err_t vui_set_pointer_observer(vui_handle_t ui, vui_pointer_cb_t callback, void *ctx);
void *vui_timer_create(vui_handle_t ui, unsigned interval, vui_timer_cb_t callback, void *ctx);
vui_err_t vui_timer_delete(vui_handle_t ui, void *timer);
