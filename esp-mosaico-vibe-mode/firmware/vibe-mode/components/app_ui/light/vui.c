// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_format.h"
#include "vui_memory.h"
#include <string.h>
#include <limits.h>

#ifdef ESP_PLATFORM
extern const uint8_t vui_assets_start[] asm("_binary_vui_assets_bin_start");
extern const uint8_t vui_assets_end[] asm("_binary_vui_assets_bin_end");
#else
extern const uint8_t vui_assets_start[];
extern const unsigned vui_assets_length;
#define vui_assets_end (vui_assets_start + vui_assets_length)
#endif
_Static_assert(sizeof(vui_node_t) == 44 && sizeof(vui_glyph_t) == 20 && sizeof(vui_font_t) == 16 &&
                   sizeof(vui_image_t) == 12,
               "asset structure layout");
#define WIDTH 480
#define HEIGHT 480
#define LISTS 2
#define ROWS 16
#define ROW_FIELDS 8
#define TIMERS 8
#define HITS 128
#define MAX_TEXT 1024

typedef struct {
    char *text;
    uint8_t *image;
    vui_dot_t *dots;
    vui_matrix_t *matrix;
    size_t dot_count, dot_capacity;
    void *canvas, *release_ctx;
    void (*release)(void *);
    size_t stride;
    uint16_t width, height, color, value;
    int scroll, marquee_extent, marquee_offset;
    uint64_t marquee_start;
    bool hidden, has_color, indeterminate, marquee_active;
} value_t;
typedef struct {
    uint16_t id;
    value_t value;
} row_field_t;
struct vui_row {
    row_field_t fields[ROW_FIELDS];
};
typedef struct {
    uint16_t id;
    unsigned total;
    int scroll;
    vui_row_cb_t callback;
    void *ctx;
    struct vui_row rows[ROWS];
} list_t;
typedef struct {
    unsigned interval;
    uint64_t next;
    vui_timer_cb_t callback;
    void *ctx;
} vui_timer_state_t;
typedef vui_area_t rect_t;
typedef struct {
    rect_t rect;
    uint16_t node;
    vui_event_t event;
} hit_t;
struct vui {
    uint8_t *assets;
    const vui_asset_header_t *header;
    const vui_node_t *nodes;
    const vui_font_t *fonts;
    const vui_image_t *images;
    value_t *values;
    int16_t *child, *sibling;
    list_t lists[LISTS];
    vui_timer_state_t timers[TIMERS];
    hit_t hits[HITS], down;
    unsigned hit_count;
    vui_event_cb_t event;
    void *event_ctx;
    vui_pointer_cb_t observer;
    void *observer_ctx;
    uint64_t now;
    uint32_t animation;
    bool dirty, pressed, moved;
    int start_x, start_y, last_y, pointer_x, pointer_y;
    uint16_t drag_node, fling_node;
    int velocity, scroll_fraction;
    uint64_t last_move;
    uint16_t *pixels, *retained;
    rect_t damage, paint_clip;
};
static int min(int a, int b)
{
    return a < b ? a : b;
}
static int max(int a, int b)
{
    return a > b ? a : b;
}
static bool valid(vui_handle_t u, unsigned id)
{
    return u && id && id <= u->header->node_count;
}
static rect_t intersect(rect_t a, rect_t b)
{
    int x = max(a.x, b.x), y = max(a.y, b.y);
    return (rect_t){x, y, max(0, min(a.x + a.w, b.x + b.w) - x),
                    max(0, min(a.y + a.h, b.y + b.h) - y)};
}
static bool contains(rect_t a, int x, int y)
{
    return x >= a.x && y >= a.y && x < a.x + a.w && y < a.y + a.h;
}
static void damage_add(vui_handle_t u, rect_t r)
{
    r = intersect(r, (rect_t){0, 0, WIDTH, HEIGHT});
    if (!r.w || !r.h) return;
    if (u->dirty) {
        int x = min(u->damage.x, r.x), y = min(u->damage.y, r.y);
        r = (rect_t){x, y, max(u->damage.x + u->damage.w, r.x + r.w) - x,
                         max(u->damage.y + u->damage.h, r.y + r.h) - y};
    }
    u->damage = r;
    u->dirty = true;
}
static void damage_node(vui_handle_t u, unsigned id)
{
    if (!valid(u, id)) return;
    int chain[33], count = 0;
    for (int i = id - 1; i >= 0; i = u->nodes[i].parent) {
        if (count == 33 || u->values[i].hidden || (u->nodes[i].flags & VUI_TEMPLATE)) return;
        chain[count++] = i;
    }
    rect_t clip = {0, 0, WIDTH, HEIGHT};
    int x = 0, y = 0;
    while (count) {
        unsigned i = chain[--count];
        const vui_node_t *n = &u->nodes[i];
        x += n->x; y += n->y;
        clip = intersect(clip, (rect_t){x, y, n->w, n->h});
        if (n->flags & VUI_SCROLL) y -= u->values[i].scroll;
    }
    damage_add(u, clip);
}
static void damage_row(vui_handle_t u, vui_row_t row)
{
    for (unsigned l = 0; l < LISTS; ++l)
        for (unsigned r = 0; r < u->lists[l].total; ++r)
            if (row == &u->lists[l].rows[r]) {
                damage_node(u, u->lists[l].id);
                return;
            }
}
static void release_value(value_t *v)
{
    if (v->text) {
        memset(v->text, 0, strlen(v->text));
        free(v->text);
    }
    free(v->image);
    free(v->dots);
    free(v->matrix);
    if (v->release)
        v->release(v->release_ctx);
    memset(v, 0, sizeof(*v));
}
static int text_value(value_t *v, const char *text, bool *changed)
{
    if (!text || strlen(text) > MAX_TEXT)
        return VUI_ERR_INVALID_ARG;
    if (v->text && !strcmp(text, v->text))
        return VUI_OK;
    size_t n = strlen(text) + 1;
    char *copy = vui_alloc(n);
    if (!copy)
        return VUI_ERR_NO_MEM;
    memcpy(copy, text, n);
    if (v->text) {
        memset(v->text, 0, strlen(v->text));
        free(v->text);
    }
    v->text = copy;
    v->marquee_active = false;
    *changed = true;
    return VUI_OK;
}
static value_t *row_value(vui_row_t row, unsigned id, bool create)
{
    for (unsigned i = 0; i < ROW_FIELDS; i++) {
        if (row->fields[i].id == id)
            return &row->fields[i].value;
        if (create && !row->fields[i].id) {
            row->fields[i].id = id;
            return &row->fields[i].value;
        }
    }
    return NULL;
}
vui_err_t vui_create(vui_handle_t *out)
{
    if (!out)
        return VUI_ERR_INVALID_ARG;
    *out = NULL;
    vui_handle_t u = vui_calloc(1, sizeof(*u));
    if (!u)
        return VUI_ERR_NO_MEM;
    uint32_t size;
    memcpy(&size, vui_assets_start, 4);
    if (size < 32 || size > 4 * 1024 * 1024) {
        free(u);
        return VUI_ERR_INVALID_ARG;
    }
    u->assets = vui_alloc(size);
    if (!u->assets)
        goto fail;
    if (!vui_inflate(vui_assets_start + 4, vui_assets_end - vui_assets_start - 4, u->assets, size))
        goto fail;
    u->header = (void *)u->assets;
    const vui_asset_header_t *h = u->header;
    if (h->magic != 0x31554956 || h->size != size || h->node_count > 2048 ||
        h->nodes + h->node_count * sizeof(vui_node_t) > size ||
        h->fonts + h->font_count * sizeof(vui_font_t) > size ||
        h->images + h->image_count * sizeof(vui_image_t) > size)
        goto fail;
    u->nodes = (void *)(u->assets + h->nodes);
    u->fonts = (void *)(u->assets + h->fonts);
    u->images = (void *)(u->assets + h->images);
    u->values = vui_calloc(h->node_count, sizeof(value_t));
    u->child = vui_alloc(h->node_count * sizeof(int16_t));
    u->sibling = vui_alloc(h->node_count * sizeof(int16_t));
    if (!u->values || !u->child || !u->sibling)
        goto fail;
    memset(u->child, 0xff, h->node_count * sizeof(int16_t));
    memset(u->sibling, 0xff, h->node_count * sizeof(int16_t));
    for (int i = (int)h->node_count - 1; i >= 0; i--) {
        const vui_node_t *n = &u->nodes[i];
        if (n->parent >= 0) {
            if (n->parent >= (int)h->node_count || n->parent == i)
                goto fail;
            u->sibling[i] = u->child[n->parent];
            u->child[n->parent] = i;
        }
        u->values[i].hidden = (n->flags & VUI_HIDDEN) != 0;
    }
    vui_invalidate(u);
    *out = u;
    return VUI_OK;
fail:
    vui_destroy(u);
    return VUI_ERR_NO_MEM;
}
void vui_destroy(vui_handle_t u)
{
    if (!u)
        return;
    if (u->values)
        for (unsigned i = 0; i < u->header->node_count; i++)
            release_value(&u->values[i]);
    for (unsigned l = 0; l < LISTS; l++)
        for (unsigned r = 0; r < ROWS; r++)
            for (unsigned f = 0; f < ROW_FIELDS; f++)
                release_value(&u->lists[l].rows[r].fields[f].value);
    free(u->values);
    free(u->child);
    free(u->sibling);
    free(u->assets);
    free(u);
}
vui_err_t vui_set_text(vui_handle_t u, uint16_t id, const char *text)
{
    if (!valid(u, id)) return VUI_ERR_INVALID_ARG;
    bool changed = false;
    int err = text_value(&u->values[id - 1], text, &changed);
    if (changed) damage_node(u, id);
    return err;
}
vui_err_t vui_set_visible(vui_handle_t u, uint16_t id, bool show)
{
    if (!valid(u, id))
        return VUI_ERR_INVALID_ARG;
    if (u->values[id - 1].hidden == show) {
        damage_node(u, id);
        u->values[id - 1].hidden = !show;
        damage_node(u, id);
    }
    return VUI_OK;
}
vui_err_t vui_component_set_visible(vui_handle_t u, uint16_t id, bool show)
{
    return vui_set_visible(u, id, show);
}
vui_err_t vui_set_value(vui_handle_t u, uint16_t id, unsigned value)
{
    if (!valid(u, id))
        return VUI_ERR_INVALID_ARG;
    value = min(value, 100);
    if (u->values[id - 1].value != value) {
        u->values[id - 1].value = value;
        damage_node(u, id);
    }
    return VUI_OK;
}
vui_err_t vui_set_indeterminate(vui_handle_t u, uint16_t id, bool active)
{
    if (!valid(u, id) || u->nodes[id - 1].kind != VUI_PROGRESS) return VUI_ERR_INVALID_ARG;
    if (u->values[id - 1].indeterminate != active) {
        u->values[id - 1].indeterminate = active;
        damage_node(u, id);
    }
    return VUI_OK;
}
static int image_value(value_t *v, void *png, size_t size, bool *changed)
{
    if (!png && !size) {
        *changed = v->image != NULL;
        free(v->image);
        v->image = NULL;
        v->marquee_active = false;
        return VUI_OK;
    }
    unsigned w, h;
    uint8_t *p = size >= 4 && png && !memcmp(png, "qoif", 4)
        ? vui_title_qoi(png, size, &w, &h) : vui_png(png, size, &w, &h);
    if (!p)
        return VUI_ERR_INVALID_ARG;
    free(v->image);
    v->image = p;
    v->marquee_active = false;
    v->width = w;
    v->height = h;
    free(png);
    *changed = true;
    return VUI_OK;
}
vui_err_t vui_set_image_owned(vui_handle_t u, uint16_t id, void *png, size_t size)
{
    if (!valid(u, id)) return VUI_ERR_INVALID_ARG;
    bool changed = false;
    int err = image_value(&u->values[id - 1], png, size, &changed);
    if (changed) damage_node(u, id);
    return err;
}
vui_err_t vui_canvas_stop(vui_handle_t u, uint16_t id)
{
    if (!valid(u, id))
        return VUI_ERR_INVALID_ARG;
    value_t *v = &u->values[id - 1];
    if (v->release)
        v->release(v->release_ctx);
    if (v->canvas)
        damage_node(u, id);
    v->canvas = NULL;
    v->release = NULL;
    v->release_ctx = NULL;
    return VUI_OK;
}
vui_err_t vui_canvas_push(vui_handle_t u, uint16_t id, void *pixels, size_t stride,
                          void (*release)(void *), void *ctx)
{
    if (!valid(u, id) || !pixels || stride < (size_t)u->nodes[id - 1].w * 2)
        return VUI_ERR_INVALID_ARG;
    vui_canvas_stop(u, id);
    value_t *v = &u->values[id - 1];
    v->canvas = pixels;
    v->stride = stride;
    v->release = release;
    v->release_ctx = ctx;
    damage_node(u, id);
    return VUI_OK;
}
vui_err_t vui_set_matrix(vui_handle_t u, uint16_t id, const vui_matrix_t *frame)
{
    if (!valid(u, id) || u->nodes[id - 1].kind != VUI_MATRIX)
        return VUI_ERR_INVALID_ARG;
    value_t *v = &u->values[id - 1];
    if (!frame) {
        if (v->matrix) { free(v->matrix); v->matrix = NULL; damage_node(u, id); }
        return VUI_OK;
    }
    if (v->matrix && !memcmp(v->matrix, frame, sizeof(*frame))) return VUI_OK;
    if (!v->matrix) {
        v->matrix = vui_alloc(sizeof(*frame));
        if (!v->matrix) return VUI_ERR_NO_MEM;
    }
    memcpy(v->matrix, frame, sizeof(*frame));
    damage_node(u, id);
    return VUI_OK;
}
vui_err_t vui_set_dots(vui_handle_t u, uint16_t id, const vui_dot_t *dots, size_t count)
{
    if (!valid(u, id) || u->nodes[id - 1].kind != VUI_DOTS ||
        count > VUI_MAX_DOTS || (count && !dots))
        return VUI_ERR_INVALID_ARG;
    /* Bound the raster arithmetic even for caller-supplied point data. */
    for (size_t i = 0; i < count; ++i)
        if (dots[i].x < -WIDTH * 16 || dots[i].x > WIDTH * 16 ||
            dots[i].y < -HEIGHT * 16 || dots[i].y > HEIGHT * 16 ||
            dots[i].radius > WIDTH * 16)
            return VUI_ERR_INVALID_ARG;
    value_t *v = &u->values[id - 1];
    if (!count) {
        if (v->dot_count) damage_node(u, id);
        free(v->dots);
        v->dots = NULL;
        v->dot_count = v->dot_capacity = 0;
        return VUI_OK;
    }
    if (count == v->dot_count && !memcmp(v->dots, dots, count * sizeof(*dots)))
        return VUI_OK;
    if (count > v->dot_capacity) {
        vui_dot_t *copy = vui_alloc(count * sizeof(*dots));
        if (!copy) return VUI_ERR_NO_MEM;
        free(v->dots);
        v->dots = copy;
        v->dot_capacity = count;
    }
    memcpy(v->dots, dots, count * sizeof(*dots));
    v->dot_count = count;
    damage_node(u, id);
    return VUI_OK;
}
vui_list_t vui_list_bind_component(vui_handle_t u, uint16_t id, vui_row_cb_t callback, void *ctx)
{
    if (!valid(u, id) || u->nodes[id - 1].kind != VUI_LIST)
        return 0;
    for (unsigned i = 0; i < LISTS; i++)
        if (!u->lists[i].id) {
            u->lists[i].id = id;
            u->lists[i].callback = callback;
            u->lists[i].ctx = ctx;
            return i + 1;
        }
    return 0;
}
static list_t *list(vui_handle_t u, vui_list_t l)
{
    return u && l && l <= LISTS && u->lists[l - 1].id ? &u->lists[l - 1] : NULL;
}
vui_err_t vui_list_refresh(vui_handle_t u, vui_list_t l)
{
    list_t *v = list(u, l);
    if (!v)
        return VUI_ERR_INVALID_ARG;
    for (unsigned i = 0; i < v->total; i++)
        if (v->callback)
            v->callback(u, &v->rows[i], i, v->ctx);
    damage_node(u, v->id);
    return VUI_OK;
}
vui_err_t vui_list_set_total(vui_handle_t u, vui_list_t l, unsigned total)
{
    list_t *v = list(u, l);
    if (!v || total > ROWS)
        return VUI_ERR_INVALID_ARG;
    if (total == v->total)
        return VUI_OK;
    for (unsigned i = total; i < v->total; i++) {
        for (unsigned j = 0; j < ROW_FIELDS; j++)
            release_value(&v->rows[i].fields[j].value);
        memset(&v->rows[i], 0, sizeof(v->rows[i]));
    }
    v->total = total;
    v->scroll =
        min(v->scroll, max(0, total * u->nodes[v->id - 1].item_height - u->nodes[v->id - 1].h));
    return vui_list_refresh(u, l);
}
vui_err_t vui_row_set_text(vui_handle_t u, vui_row_t row, uint16_t id, const char *text)
{
    value_t *v = row ? row_value(row, id, true) : NULL;
    if (!v) return VUI_ERR_INVALID_ARG;
    bool changed = false;
    int err = text_value(v, text, &changed);
    if (changed) damage_row(u, row);
    return err;
}
vui_err_t vui_row_set_color(vui_handle_t u, vui_row_t row, uint16_t id, uint16_t color)
{
    value_t *v = row ? row_value(row, id, true) : NULL;
    if (!v)
        return VUI_ERR_INVALID_ARG;
    if (!v->has_color || v->color != color) damage_row(u, row);
    v->color = color;
    v->has_color = true;
    return VUI_OK;
}
vui_err_t vui_row_set_image_owned(vui_handle_t u, vui_row_t row, uint16_t id, void *png,
                                  size_t size)
{
    value_t *v = row ? row_value(row, id, true) : NULL;
    if (!v) return VUI_ERR_INVALID_ARG;
    bool changed = false;
    int err = image_value(v, png, size, &changed);
    if (changed) damage_row(u, row);
    return err;
}
vui_err_t vui_on_event(vui_handle_t u, vui_event_cb_t cb, void *ctx)
{
    if (!u)
        return VUI_ERR_INVALID_ARG;
    u->event = cb;
    u->event_ctx = ctx;
    return VUI_OK;
}
vui_err_t vui_set_pointer_observer(vui_handle_t u, vui_pointer_cb_t cb, void *ctx)
{
    if (!u)
        return VUI_ERR_INVALID_ARG;
    u->observer = cb;
    u->observer_ctx = ctx;
    return VUI_OK;
}
void *vui_timer_create(vui_handle_t u, unsigned interval, vui_timer_cb_t cb, void *ctx)
{
    if (!u || !interval || !cb)
        return NULL;
    for (unsigned i = 0; i < TIMERS; i++)
        if (!u->timers[i].callback) {
            u->timers[i] = (vui_timer_state_t){interval, u->now + interval, cb, ctx};
            return &u->timers[i];
        }
    return NULL;
}
vui_err_t vui_timer_delete(vui_handle_t u, void *timer)
{
    if (!u || !timer)
        return VUI_ERR_INVALID_ARG;
    for (unsigned i = 0; i < TIMERS; i++)
        if (timer == &u->timers[i]) {
            memset(timer, 0, sizeof(vui_timer_state_t));
            return VUI_OK;
        }
    return VUI_ERR_INVALID_ARG;
}
static bool visible(vui_handle_t u, int i)
{
    for (unsigned depth = 0; i >= 0 && depth < u->header->node_count; depth++) {
        if (u->values[i].hidden || (u->nodes[i].flags & VUI_TEMPLATE))
            return false;
        i = u->nodes[i].parent;
    }
    return i < 0;
}
static int *scroll_position(vui_handle_t u, uint16_t id, int *limit)
{
    if (!valid(u, id)) return NULL;
    const vui_node_t *n = &u->nodes[id - 1];
    if (n->flags & VUI_SCROLL) {
        *limit = max(0, n->content_height - n->h);
        return &u->values[id - 1].scroll;
    }
    if (n->kind == VUI_LIST)
        for (unsigned i = 0; i < LISTS; ++i)
            if (u->lists[i].id == id) {
                *limit = max(0, (int)u->lists[i].total * n->item_height - n->h);
                return &u->lists[i].scroll;
            }
    return NULL;
}
vui_err_t vui_scroll_to(vui_handle_t u, uint16_t id, int offset)
{
    int limit;
    int *position = scroll_position(u, id, &limit);
    if (!position) return VUI_ERR_INVALID_ARG;
    offset = max(0, min(offset, limit));
    if (*position != offset) { *position = offset; damage_node(u, id); }
    if (u->fling_node == id) u->fling_node = 0;
    return VUI_OK;
}
int vui_scroll_offset(vui_handle_t u, uint16_t id)
{
    int limit;
    int *position = scroll_position(u, id, &limit);
    return position ? *position : 0;
}
uint64_t vui_now_ms(vui_handle_t u)
{
    return u ? u->now : 0;
}
/* Native title movement: 28 px/s, with a 1.2 s reading pause at both ends.
 * Only an overflowing, visible title damages the retained framebuffer. */
static int marquee_offset(const value_t *v, uint64_t now)
{
    if (!v->marquee_active || v->marquee_extent <= 0) return 0;
    uint64_t travel = ((uint64_t)v->marquee_extent * 1000 + 27) / 28;
    uint64_t t = (now - v->marquee_start) % (2 * (1200 + travel));
    if (t < 1200) return 0;
    t -= 1200;
    if (t < travel) return (int)(t * v->marquee_extent / travel);
    t -= travel;
    if (t < 1200) return v->marquee_extent;
    t -= 1200;
    return v->marquee_extent - (int)(t * v->marquee_extent / travel);
}
static int marquee_layout(vui_handle_t u, value_t *v, int content, int viewport)
{
    int extent = max(0, content - viewport);
    if (!v->marquee_active || v->marquee_extent != extent) {
        v->marquee_start = u->now;
        v->marquee_offset = 0;
        v->marquee_extent = extent;
        v->marquee_active = true;
    }
    return v->marquee_offset;
}
void vui_tick(vui_handle_t u, uint32_t elapsed)
{
    if (!u)
        return;
    uint64_t previous = u->now;
    u->now += elapsed;
    for (unsigned i = 0; i < TIMERS; i++) {
        vui_timer_state_t *t = &u->timers[i];
        if (t->callback && u->now >= t->next) {
            t->next = u->now + t->interval;
            t->callback(u, t->ctx);
        }
    }
    /* Time-based deceleration, bounded even after a long stalled frame. */
    for (unsigned remaining = elapsed > 200 ? 200 : elapsed; u->fling_node && remaining;) {
        unsigned step = min(remaining, 16);
        remaining -= step;
        int limit;
        int *position = scroll_position(u, u->fling_node, &limit);
        if (!position || !visible(u, u->fling_node - 1)) { u->fling_node = 0; break; }
        u->scroll_fraction += u->velocity * (int)step;
        int next = max(0, min(*position + u->scroll_fraction / 1000, limit));
        u->scroll_fraction %= 1000;
        if (next != *position) { *position = next; damage_node(u, u->fling_node); }
        u->velocity = u->velocity * (1000 - (int)step * 6) / 1000;
        if (!next || next == limit || abs(u->velocity) < 20) u->fling_node = 0;
    }
    for (unsigned l = 0; l < LISTS; ++l) {
        list_t *list = &u->lists[l];
        bool shown = list->id && visible(u, list->id - 1);
        for (unsigned r = 0; r < list->total; ++r) {
            int y = r * u->nodes[list->id - 1].item_height - list->scroll;
            bool on_screen = shown && y + u->nodes[list->id - 1].item_height > 0 &&
                             y < u->nodes[list->id - 1].h;
            for (unsigned f = 0; f < ROW_FIELDS; ++f) {
                row_field_t *field = &list->rows[r].fields[f];
                if (!valid(u, field->id) || !(u->nodes[field->id - 1].flags & VUI_MARQUEE)) continue;
                value_t *v = &field->value;
                if (!on_screen) v->marquee_active = false;
                int offset = marquee_offset(v, u->now);
                if (offset != v->marquee_offset) {
                    v->marquee_offset = offset;
                    if (on_screen) damage_node(u, list->id);
                }
            }
        }
    }
    for (unsigned i = 0; i < u->header->node_count; i++) {
        if (u->nodes[i].flags & VUI_MARQUEE) {
            value_t *v = &u->values[i];
            bool shown = visible(u, i);
            if (!shown) v->marquee_active = false;
            int offset = marquee_offset(v, u->now);
            if (offset != v->marquee_offset) {
                v->marquee_offset = offset;
                if (shown) damage_node(u, i + 1);
            }
        }
        if (u->values[i].indeterminate && previous / 50 != u->now / 50)
            damage_node(u, i + 1);
        if (u->nodes[i].image) {
            const vui_image_t *im = &u->images[u->nodes[i].image - 1];
            if (im->frames > 1 && visible(u, i) && u->now / im->duration != u->animation) {
                u->animation = u->now / im->duration;
                damage_node(u, i + 1);
            }
        }
    }
}
void vui_invalidate(vui_handle_t u)
{
    if (u)
        damage_add(u, (rect_t){0, 0, WIDTH, HEIGHT});
}
static uint16_t blend(uint16_t dst, uint16_t src, unsigned a)
{
    if (a == 255)
        return src;
    unsigned r = (((src >> 11) * a + (dst >> 11) * (255 - a) + 127) / 255),
             g = ((((src >> 5) & 63) * a + ((dst >> 5) & 63) * (255 - a) + 127) / 255),
             b = (((src & 31) * a + (dst & 31) * (255 - a) + 127) / 255);
    return (r << 11) | (g << 5) | b;
}
static void pixel(vui_handle_t u, int x, int y, uint16_t color, unsigned alpha)
{
    uint16_t *p = &u->pixels[y * WIDTH + x];
    *p = alpha == 255 ? color : blend(*p, color, alpha);
}
static unsigned coverage(rect_t r, int radius, int x, int y)
{
    if (!contains(r, x, y))
        return 0;
    int rad = min(radius, min(r.w, r.h) / 2);
    int dx = x < r.x + rad ? r.x + rad - x - 1 : x >= r.x + r.w - rad ? x - (r.x + r.w - rad) : 0;
    int dy = y < r.y + rad ? r.y + rad - y - 1 : y >= r.y + r.h - rad ? y - (r.y + r.h - rad) : 0;
    if (!rad || !dx || !dy)
        return 255;
    unsigned count = 0;
    for (int yy = 1; yy < 4; yy += 2)
        for (int xx = 1; xx < 4; xx += 2)
            if ((dx * 4 + xx) * (dx * 4 + xx) + (dy * 4 + yy) * (dy * 4 + yy) <= rad * rad * 16)
                count++;
    return count * 255 / 4;
}
static void fill(uint16_t *pixels, int count, uint16_t color)
{
    if (count <= 0) return;
    if ((color & 255) == (color >> 8)) memset(pixels, color & 255, (size_t)count * 2);
    else for (int x = 0; x < count; ++x) pixels[x] = color;
}
static void box(vui_handle_t u, rect_t r, rect_t clip, unsigned radius, uint16_t color)
{
    rect_t b = intersect(r, clip);
    int rad = min(radius, min(r.w, r.h) / 2);
    for (int y = b.y; y < b.y + b.h; y++) {
        int left = b.x, right = b.x + b.w;
        if (rad && (y < r.y + rad || y >= r.y + r.h - rad)) {
            left = min(right, max(left, r.x + rad));
            right = max(left, min(right, r.x + r.w - rad));
            for (int x = b.x; x < left; ++x) {
                unsigned a = coverage(r, rad, x, y);
                if (a) pixel(u, x, y, color, a);
            }
            for (int x = right; x < b.x + b.w; ++x) {
                unsigned a = coverage(r, rad, x, y);
                if (a) pixel(u, x, y, color, a);
            }
        }
        fill(u->pixels + y * WIDTH + left, right - left, color);
    }
}
static void outline(vui_handle_t u, rect_t r, rect_t clip, unsigned radius, unsigned width,
                    uint16_t color)
{
    rect_t b = intersect(r, clip),
           inner = {r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width};
    int rad = max(0, (int)radius - (int)width);
    int inner_rad = min(rad, min(inner.w, inner.h) / 2);
    for (int y = b.y; y < b.y + b.h; y++)
        for (int x = b.x; x < b.x + b.w; x++) {
            /* The opaque interior contributes no border pixels. */
            if (inner.w > 0 && inner.h > 0 && y >= inner.y && y < inner.y + inner.h &&
                x >= inner.x + inner_rad && x < inner.x + inner.w - inner_rad) {
                x = min(b.x + b.w, inner.x + inner.w - inner_rad) - 1;
                continue;
            }
            int a = (int)coverage(r, radius, x, y) - (int)coverage(inner, rad, x, y);
            if (a > 0) pixel(u, x, y, color, a);
        }
}
static uint32_t utf8(const char **s)
{
    const unsigned char *p = (const void *)*s;
    unsigned c = *p++;
    if (c >= 0xc2 && c <= 0xf4) {
        unsigned n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        c &= (1U << (6 - n)) - 1;
        while (n--) {
            if ((*p & 0xc0) != 0x80) {
                c = '?';
                break;
            }
            c = (c << 6) | (*p++ & 63);
        }
    } else if (c >= 128)
        c = '?';
    *s = (const char *)p;
    return c;
}
static const vui_glyph_t *glyph(vui_handle_t u, const vui_font_t *f, unsigned code)
{
    const vui_glyph_t *g = (void *)(u->assets + f->glyphs);
    unsigned lo = 0, hi = f->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g[mid].code < code)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < f->count && g[lo].code == code)
        return &g[lo];
    if (code != '?')
        return glyph(u, f, '?');
    return NULL;
}
static void text(vui_handle_t u, const vui_node_t *n, rect_t r, rect_t clip, const char *s,
                 uint16_t color)
{
    if (!s || !*s)
        return;
    const vui_font_t *f = &u->fonts[n->font];
    /* Fixed text boxes wrap at spaces (and between CJK glyphs), preserving
     * explicit line breaks. A final overflowing line receives an ellipsis. */
    const char *starts[32], *ends[32];
    int lengths[32], lines = 0;
    bool dots[32] = {0};
    int max_lines = (n->flags & VUI_ELLIPSIS) ? 1 : min(32, max(1, r.h / f->height));
    const vui_glyph_t *period = glyph(u, f, '.');
    int ellipsis = period ? period->advance * 3 : 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        const char *start = p, *space = NULL;
        int width = 0, spacewidth = 0;
        while (*p && *p != '\n') {
            const char *before = p;
            unsigned c = utf8(&p);
            const vui_glyph_t *g = glyph(u, f, c);
            int adv = g ? g->advance : 0;
            if (width + adv > r.w * 64 && before > start) {
                p = space ? space + 1 : before;
                width = space ? spacewidth : width;
                break;
            }
            if (c == ' ') {
                space = before;
                spacewidth = width;
            }
            width += adv;
        }
        starts[lines] = start;
        ends[lines] = space && p == space + 1 ? space : p;
        lengths[lines] = width;
        if (lines == max_lines - 1 && *p && *p != '\n') {
            dots[lines] = true;
            const char *end = start;
            int fit = 0;
            while (end < ends[lines]) {
                const char *before = end;
                unsigned c = utf8(&end);
                const vui_glyph_t *g = glyph(u, f, c);
                int adv = g ? g->advance : 0;
                if (fit + adv + ellipsis > r.w * 64) {
                    end = before;
                    break;
                }
                fit += adv;
            }
            ends[lines] = end;
            lengths[lines] = fit + ellipsis;
        }
        lines++;
        if (*p == '\n')
            p++;
    }
    int lineh = f->height, baseline = r.y + max(0, (r.h - lines * lineh) / 2) + f->ascent;
    clip = intersect(clip, r);
    for (int l = 0; l < lines && baseline - f->ascent < r.y + r.h; l++, baseline += lineh) {
        int pen = r.x * 64 + (n->align == 1   ? (r.w * 64 - lengths[l]) / 2
                              : n->align == 2 ? r.w * 64 - lengths[l]
                                              : 0);
        p = starts[l];
        unsigned extra = dots[l] ? 3 : 0;
        while (p < ends[l] || extra) {
            unsigned code = p < ends[l] ? utf8(&p) : '.';
            if (code == '.' && p >= ends[l] && extra)
                extra--;
            const vui_glyph_t *g = glyph(u, f, code);
            if (!g)
                continue;
            rect_t gr = {pen / 64 + g->x, baseline + g->y, g->w, g->h}, b = intersect(gr, clip);
            const uint8_t *bits = u->assets + g->data;
            for (int y = b.y; y < b.y + b.h; y++)
                for (int x = b.x; x < b.x + b.w; x++) {
                    unsigned a = bits[(y - gr.y) * g->w + x - gr.x];
                    if (a)
                        pixel(u, x, y, color, a);
                }
            pen += g->advance;
        }
    }
}
static void marquee_text(vui_handle_t u, const vui_node_t *n, value_t *v,
                          rect_t r, rect_t clip, const char *s, uint16_t color)
{
    if (!s || !*s) return;
    const vui_font_t *f = &u->fonts[n->font];
    int width = 0;
    for (const char *p = s; *p;) {
        unsigned code = utf8(&p);
        const vui_glyph_t *g = glyph(u, f, code == '\n' ? ' ' : code);
        if (g) width += g->advance;
    }
    int offset = marquee_layout(u, v, (width + 63) / 64, r.w);
    int pen = (r.x - offset) * 64, baseline = r.y + max(0, (r.h - f->height) / 2) + f->ascent;
    for (const char *p = s; *p;) {
        unsigned code = utf8(&p);
        const vui_glyph_t *g = glyph(u, f, code == '\n' ? ' ' : code);
        if (!g) continue;
        rect_t gr = {pen / 64 + g->x, baseline + g->y, g->w, g->h}, b = intersect(gr, clip);
        const uint8_t *bits = u->assets + g->data;
        for (int y = b.y; y < b.y + b.h; ++y)
            for (int x = b.x; x < b.x + b.w; ++x) {
                unsigned alpha = bits[(y - gr.y) * g->w + x - gr.x];
                if (alpha) pixel(u, x, y, color, alpha);
            }
        pen += g->advance;
    }
}
static void bitmap(vui_handle_t u, rect_t r, rect_t clip, const uint8_t *data, unsigned w,
                   unsigned h, bool rgb, size_t stride, unsigned radius)
{
    if (!w || !h)
        return;
    rect_t b = intersect(r, clip);
    bool native = (unsigned)r.w == w && (unsigned)r.h == h;
    for (int y = b.y; y < b.y + b.h; y++) {
        unsigned sy = native ? (unsigned)(y - r.y) : (y - r.y) * h / r.h;
        const uint8_t *row = data + sy * stride;
        if (native && rgb && !radius) {
            memcpy(u->pixels + y * WIDTH + b.x, row + (b.x - r.x) * 2, (size_t)b.w * 2);
            continue;
        }
        for (int x = b.x; x < b.x + b.w; x++) {
            unsigned sx = native ? (unsigned)(x - r.x) : (x - r.x) * w / r.w;
            const uint8_t *p = row + sx * (rgb ? 2 : 3);
            unsigned a = rgb ? 255 : p[2];
            if (radius) a = a * coverage(r, radius, x, y) / 255;
            if (a) pixel(u, x, y, p[0] | (p[1] << 8), a);
        }
    }
}
static void draw_dots(vui_handle_t u, rect_t r, rect_t clip, const value_t *v)
{
    for (size_t i = 0; i < v->dot_count; ++i) {
        const vui_dot_t *d = &v->dots[i];
        if (!d->radius || !d->brightness) continue;
        int cx = r.x * 16 + d->x, cy = r.y * 16 + d->y;
        int outer = d->radius + 8, inner = max(0, (int)d->radius - 8);
        int outer2 = outer * outer, inner2 = inner * inner;
        rect_t bounds = {max(clip.x, (cx - outer) / 16),
                         max(clip.y, (cy - outer) / 16), 0, 0};
        int right = min(clip.x + clip.w, (cx + outer + 15) / 16);
        int bottom = min(clip.y + clip.h, (cy + outer + 15) / 16);
        unsigned red = (d->red * d->brightness + 127) / 255;
        unsigned green = (d->green * d->brightness + 127) / 255;
        unsigned blue = (d->blue * d->brightness + 127) / 255;
        uint16_t color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
        for (int y = bounds.y; y < bottom; ++y) {
            int dy = y * 16 + 8 - cy;
            for (int x = bounds.x; x < right; ++x) {
                int dx = x * 16 + 8 - cx;
                int distance2 = dx * dx + dy * dy;
                if (distance2 >= outer2) continue;
                unsigned alpha = distance2 <= inner2 ? 255 :
                    (unsigned)(outer2 - distance2) * 255 / (outer2 - inner2);
                pixel(u, x, y, color, alpha);
            }
        }
    }
}
static void hit(vui_handle_t u, rect_t r, rect_t clip, unsigned node, vui_event_t event)
{
    r = intersect(r, clip);
    if (r.w > 0 && r.h > 0 && u->hit_count < HITS)
        u->hits[u->hit_count++] = (hit_t){r, node, event};
}
static void draw_matrix(vui_handle_t u, rect_t r, rect_t clip, const value_t *v)
{
    box(u, r, clip, r.w / 7, 0x1082);
    outline(u, r, clip, r.w / 7, 1, 0x2945);
    int pitch = (r.w - 20) / 15, cell = max(1, pitch - 2);
    int ox = r.x + (r.w - (15 * pitch - 2)) / 2;
    int oy = r.y + (r.h - (15 * pitch - 2)) / 2;
    for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x) {
        int edge_x = min(x, 14-x), edge_y = min(y, 14-y);
        if (edge_x + edge_y < 2) continue;
        int i = y * 15 + x;
        unsigned w = v->matrix ? v->matrix->white[i] : 0;
        unsigned a = v->matrix ? v->matrix->accent[i] : 0;
        unsigned level = max(w, a);
        unsigned red = (25 * (255-level) + 243 * w + 255 * a) / 255;
        unsigned green = (27 * (255-level) + 244 * w + 100 * a) / 255;
        unsigned blue = (29 * (255-level) + 242 * w + 57 * a) / 255;
        uint16_t color = ((min(255,red) >> 3) << 11) | ((min(255,green) >> 2) << 5) | (min(255,blue) >> 3);
        box(u, (rect_t){ox+x*pitch,oy+y*pitch,cell,cell}, clip, 0, color);
    }
}
static rect_t key_preview(vui_handle_t u)
{
    rect_t r = u->down.rect;
    if (!valid(u, u->down.node) || u->down.event.arg < 33 || u->down.event.arg > 126)
        return (rect_t){0};
    int parent = u->nodes[u->down.node - 1].parent;
    if (parent < 0 || !(u->nodes[parent].flags & VUI_KEY_MODE)) return (rect_t){0};
    return (rect_t){max(4, min(414, r.x + r.w/2 - 31)), r.y - 78, 62, 72};
}
static void draw(vui_handle_t u, int i, int x, int y, rect_t clip, vui_row_t row, unsigned depth)
{
    if (depth > 32 || u->values[i].hidden)
        return;
    const vui_node_t *n = &u->nodes[i];
    rect_t r = {x + n->x, y + n->y, n->w, n->h};
    clip = intersect(clip, r);
    if (!clip.w || !clip.h)
        return;
    value_t *v = row ? row_value(row, i + 1, false) : NULL;
    if (!v)
        v = &u->values[i];
    uint16_t fg = v->has_color ? v->color : n->fg;
    rect_t paint = intersect(clip, u->paint_clip);
    if (paint.w && paint.h) {
        if (n->flags & VUI_BACKGROUND)
            box(u, r, paint, n->radius, n->bg);
        if (n->border_width)
            outline(u, r, paint, n->radius, n->border_width, n->border);
        if (n->kind == VUI_DOTS)
            draw_dots(u, r, paint, v);
        if (n->kind == VUI_MATRIX)
            draw_matrix(u, r, paint, v);
        if (n->kind == VUI_PROGRESS) {
            rect_t bar = {r.x, r.y, r.w * v->value / 100, r.h};
            if (v->indeterminate) {
                unsigned step = (u->now / 50) % 40;
                bar.w = max(1, r.w / 4);
                bar.x += (r.w - bar.w) * (int)(step <= 20 ? step : 40 - step) / 20;
            }
            box(u, bar, paint, n->radius, fg);
        }
        if (n->kind == VUI_IMAGE) {
            if (v->canvas)
                bitmap(u, r, paint, v->canvas, n->w, n->h, true, v->stride, n->radius);
            else if (v->image) {
                rect_t image = r;
                if (n->flags & VUI_MARQUEE) {
                    /* Preserve aspect ratio. Never enlarge a short title to
                     * fill its box; unusually tall fonts may shrink uniformly. */
                    image.h = min(r.h, v->height);
                    image.w = max(1, (int)v->width * image.h / v->height);
                    image.y += (r.h - image.h) / 2;
                    image.x -= marquee_layout(u, v, image.w, r.w);
                }
                bitmap(u, image, paint, v->image, v->width, v->height, false, v->width * 3, n->radius);
            }
            else if (n->image) {
                const vui_image_t *im = &u->images[n->image - 1];
                bitmap(u, r, paint,
                       u->assets + im->data + (u->now / im->duration % im->frames) * im->w * im->h * 3,
                       im->w, im->h, false, im->w * 3, n->radius);
            }
        }
        if (n->kind == VUI_LABEL || n->kind == VUI_BUTTON) {
            const char *label = v->text ? v->text : (const char *)u->assets + n->text;
            if (n->flags & VUI_MARQUEE) marquee_text(u, n, v, r, paint, label, fg);
            else text(u, n, r, paint, label, fg);
        }
    }
    if (n->kind == VUI_BUTTON)
        hit(u, r, clip, i + 1,
            (vui_event_t){.type = VUI_EVENT_CALL, .action_id = n->action, .arg = n->arg});
    if ((n->flags & VUI_SCROLL) || n->kind == VUI_LIST)
        hit(u, r, clip, i + 1, (vui_event_t){0});
    if (n->kind == VUI_LIST) {
        for (unsigned l = 0; l < LISTS; l++)
            if (u->lists[l].id == i + 1) {
                list_t *lst = &u->lists[l];
                for (unsigned j = 0; j < lst->total; j++) {
                    int yy = r.y + j * n->item_height - lst->scroll;
                    if (yy + n->item_height <= clip.y || yy >= clip.y + clip.h)
                        continue;
                    int t = n->template_node - 1;
                    if (t < 0)
                        continue;
                    draw(u, t, r.x, yy, clip, &lst->rows[j], depth + 1);
                    hit(u, (rect_t){r.x, yy, r.w, n->item_height}, clip, i + 1,
                        (vui_event_t){.type = VUI_EVENT_CALL,
                                      .action_id = u->nodes[t].action,
                                      .list = l + 1,
                                      .item = j});
                }
            }
    }
    int offset = (n->flags & VUI_SCROLL) ? v->scroll : 0;
    for (int c = u->child[i]; c >= 0; c = u->sibling[c])
        draw(u, c, r.x, r.y - offset, clip, row, depth + 1);
    if (n->kind == VUI_BUTTON && u->pressed && !u->moved && u->down.node == i + 1 &&
        contains(u->down.rect, u->pointer_x, u->pointer_y)) {
        if (n->flags & VUI_PRESS_RING) {
            /* Grid presses keep the icon in place and only light its ring. */
            int diameter = r.w - 8;
            outline(u, (rect_t){r.x + 4, r.y + 4, diameter, diameter}, paint, diameter / 2, 2, 0xf79e);
        } else {
            /* A light overlay preserves the card geometry and all child labels. */
            for (int yy = paint.y; yy < paint.y + paint.h; ++yy)
                for (int xx = paint.x; xx < paint.x + paint.w; ++xx)
                    pixel(u, xx, yy, 0xffff, coverage(r, n->radius ? n->radius : 18, xx, yy) * 10 / 255);
        }
    }
    if ((n->flags & VUI_SCROLL) && n->content_height > n->h) {
        int track = r.h - 8, thumb = max(20, track * r.h / n->content_height);
        int y = r.y + 4 + offset * (track - thumb) / (n->content_height - n->h);
        box(u, (rect_t){r.x + r.w - 12, r.y + 4, 4, track}, paint, 2, 0x2945);
        box(u, (rect_t){r.x + r.w - 12, y, 4, thumb}, paint, 2, 0x9cd3);
    }
}
bool vui_render_changed(vui_handle_t u, uint16_t *pixels, vui_area_t *changed)
{
    if (!u || !pixels) return false;
    if (u->retained != pixels) vui_invalidate(u);
    if (!u->dirty) return false;
    u->pixels = u->retained = pixels;
    u->paint_clip = u->damage;
    if (changed) *changed = u->damage;
    u->dirty = false;
    u->damage = (rect_t){0};
    u->hit_count = 0;
    for (int y = u->paint_clip.y; y < u->paint_clip.y + u->paint_clip.h; ++y)
        memset(pixels + y * WIDTH + u->paint_clip.x, 0, (size_t)u->paint_clip.w * 2);
    /* Rebuild all hit regions using geometry clips, independently of the
     * paint clip: a small animation update must not remove other buttons. */
    for (unsigned i = 0; i < u->header->node_count; i++)
        if (u->nodes[i].parent < 0 && !(u->nodes[i].flags & VUI_TEMPLATE))
            draw(u, i, 0, 0, (rect_t){0, 0, WIDTH, HEIGHT}, NULL, 0);
    if (u->pressed && !u->moved && valid(u, u->down.node) && visible(u, u->down.node-1) &&
        contains(u->down.rect, u->pointer_x, u->pointer_y)) {
        rect_t bubble = key_preview(u), clip = intersect(bubble, u->paint_clip);
        if (bubble.w && clip.w && clip.h) {
            box(u, bubble, clip, 10, 0xf79e);
            vui_node_t key = u->nodes[u->down.node-1];
            key.align = 1;
            char value[2] = {(char)u->down.event.arg, 0};
            text(u, &key, bubble, clip, value, 0x0841);
        }
    }
    u->pixels = NULL;
    return true;
}
bool vui_render(vui_handle_t u, uint16_t *pixels)
{
    return vui_render_changed(u, pixels, NULL);
}
void vui_pointer(vui_handle_t u, int x, int y, bool pressed)
{
    if (u) vui_pointer_at(u, x, y, pressed, u->now);
}
void vui_pointer_at(vui_handle_t u, int x, int y, bool pressed, uint64_t time_ms)
{
    if (!u)
        return;
    if (u->pressed) { damage_node(u, u->down.node); damage_add(u, key_preview(u)); }
    u->pointer_x = x; u->pointer_y = y;
    if (pressed && !u->pressed) {
        memset(&u->down, 0, sizeof(u->down));
        u->start_x = x;
        u->start_y = y;
        u->last_y = y;
        uint16_t coasting = u->fling_node;
        u->moved = false;
        u->fling_node = 0;
        u->velocity = u->scroll_fraction = 0;
        u->last_move = time_ms;
        u->drag_node = 0;
        for (int i = (int)u->hit_count - 1; i >= 0; i--)
            if (contains(u->hits[i].rect, x, y)) {
                u->down = u->hits[i];
                for (int node = u->down.node - 1; node >= 0; node = u->nodes[node].parent)
                    if ((u->nodes[node].flags & VUI_SCROLL) || u->nodes[node].kind == VUI_LIST) {
                        u->drag_node = node + 1;
                        break;
                    }
                break;
            }
        /* A tap on moving content stops it; fixed header controls still act
         * immediately, even while the content below them is coasting. */
        u->moved = coasting && coasting == u->drag_node;
    }
    if (u->pressed) {
        if (abs(x - u->start_x) > 12 || abs(y - u->start_y) > 12)
            u->moved = true;
        int limit;
        int *position = scroll_position(u, u->drag_node, &limit);
        int delta = u->last_y - y;
        if (position && u->moved && delta) {
            int next = max(0, min(*position + delta, limit));
            uint64_t elapsed_move = time_ms >= u->last_move ? time_ms - u->last_move : 0;
            int dt = elapsed_move > 200 ? 200 : max(1, (int)elapsed_move);
            u->velocity = max(-3000, min(3000, (next - *position) * 1000 / dt));
            u->last_move = time_ms;
            if (next != *position) { *position = next; damage_node(u, u->drag_node); }
        }
        u->last_y = y;
    }
    bool clicked =
        !pressed && u->pressed && !u->moved && u->down.node && contains(u->down.rect, x, y);
    if (!pressed && u->pressed && u->drag_node && u->moved &&
        time_ms >= u->last_move && time_ms - u->last_move < 120 && abs(u->velocity) >= 20)
        u->fling_node = u->drag_node;
    if (u->observer)
        u->observer(u, x, y, pressed, u->observer_ctx);
    if (clicked && visible(u, u->down.node - 1)) {
        if (u->down.event.arg >= 0x100 && u->down.event.arg <= 0x103) {
            int layer = u->nodes[u->down.node - 1].parent, keyboard = u->nodes[layer].parent,
                mode = u->down.event.arg == 0x100   ? 1
                       : u->down.event.arg == 0x101 ? 0
                       : u->down.event.arg == 0x102 ? 2 : 3;
            for (int c = u->child[keyboard], m = 0; c >= 0; c = u->sibling[c])
                if (u->nodes[c].flags & VUI_KEY_MODE)
                    vui_set_visible(u, c + 1, m++ == mode);
        } else if (u->event)
            u->event(u, &u->down.event, u->event_ctx);
    }
    u->pressed = pressed;
    if (pressed) { damage_node(u, u->down.node); damage_add(u, key_preview(u)); }
}
