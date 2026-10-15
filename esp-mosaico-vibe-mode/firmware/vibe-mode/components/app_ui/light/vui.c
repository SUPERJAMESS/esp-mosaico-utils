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
    void *canvas, *release_ctx;
    void (*release)(void *);
    size_t stride;
    uint16_t width, height, color, value;
    int scroll;
    bool hidden, has_color;
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
typedef struct {
    int x, y, w, h;
} rect_t;
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
    int start_x, start_y, last_y;
    uint16_t drag_node, fling_node;
    int velocity, scroll_fraction;
    uint64_t last_move;
    uint16_t *pixels;
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
static void release_value(value_t *v)
{
    if (v->text) {
        memset(v->text, 0, strlen(v->text));
        free(v->text);
    }
    free(v->image);
    if (v->release)
        v->release(v->release_ctx);
    memset(v, 0, sizeof(*v));
}
static int text_value(vui_handle_t u, value_t *v, const char *text)
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
    u->dirty = true;
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
    u->dirty = true;
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
    return valid(u, id) ? text_value(u, &u->values[id - 1], text) : VUI_ERR_INVALID_ARG;
}
vui_err_t vui_set_visible(vui_handle_t u, uint16_t id, bool show)
{
    if (!valid(u, id))
        return VUI_ERR_INVALID_ARG;
    if (u->values[id - 1].hidden == show) {
        u->values[id - 1].hidden = !show;
        u->dirty = true;
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
        u->dirty = true;
    }
    return VUI_OK;
}
static int image_value(vui_handle_t u, value_t *v, void *png, size_t size)
{
    if (!png && !size) {
        free(v->image);
        v->image = NULL;
        u->dirty = true;
        return VUI_OK;
    }
    unsigned w, h;
    uint8_t *p = size >= 4 && png && !memcmp(png, "qoif", 4)
        ? vui_title_qoi(png, size, &w, &h) : vui_png(png, size, &w, &h);
    if (!p)
        return VUI_ERR_INVALID_ARG;
    free(v->image);
    v->image = p;
    v->width = w;
    v->height = h;
    free(png);
    u->dirty = true;
    return VUI_OK;
}
vui_err_t vui_set_image_owned(vui_handle_t u, uint16_t id, void *png, size_t size)
{
    return valid(u, id) ? image_value(u, &u->values[id - 1], png, size) : VUI_ERR_INVALID_ARG;
}
vui_err_t vui_canvas_stop(vui_handle_t u, uint16_t id)
{
    if (!valid(u, id))
        return VUI_ERR_INVALID_ARG;
    value_t *v = &u->values[id - 1];
    if (v->release)
        v->release(v->release_ctx);
    if (v->canvas)
        u->dirty = true;
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
    u->dirty = true;
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
    u->dirty = true;
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
    return v ? text_value(u, v, text) : VUI_ERR_INVALID_ARG;
}
vui_err_t vui_row_set_color(vui_handle_t u, vui_row_t row, uint16_t id, uint16_t color)
{
    value_t *v = row ? row_value(row, id, true) : NULL;
    if (!v)
        return VUI_ERR_INVALID_ARG;
    v->color = color;
    v->has_color = true;
    u->dirty = true;
    return VUI_OK;
}
vui_err_t vui_row_set_image_owned(vui_handle_t u, vui_row_t row, uint16_t id, void *png,
                                  size_t size)
{
    value_t *v = row ? row_value(row, id, true) : NULL;
    return v ? image_value(u, v, png, size) : VUI_ERR_INVALID_ARG;
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
    if (*position != offset) { *position = offset; u->dirty = true; }
    if (u->fling_node == id) u->fling_node = 0;
    return VUI_OK;
}
int vui_scroll_offset(vui_handle_t u, uint16_t id)
{
    int limit;
    int *position = scroll_position(u, id, &limit);
    return position ? *position : 0;
}
void vui_tick(vui_handle_t u, uint32_t elapsed)
{
    if (!u)
        return;
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
        if (next != *position) { *position = next; u->dirty = true; }
        u->velocity = u->velocity * (1000 - (int)step * 6) / 1000;
        if (!next || next == limit || abs(u->velocity) < 20) u->fling_node = 0;
    }
    for (unsigned i = 0; i < u->header->node_count; i++)
        if (u->nodes[i].image) {
            const vui_image_t *im = &u->images[u->nodes[i].image - 1];
            if (im->frames > 1 && visible(u, i) && u->now / im->duration != u->animation) {
                u->animation = u->now / im->duration;
                u->dirty = true;
            }
        }
}
void vui_invalidate(vui_handle_t u)
{
    if (u)
        u->dirty = true;
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
    *p = blend(*p, color, alpha);
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
static void box(vui_handle_t u, rect_t r, rect_t clip, unsigned radius, uint16_t color)
{
    rect_t b = intersect(r, clip);
    for (int y = b.y; y < b.y + b.h; y++)
        for (int x = b.x; x < b.x + b.w; x++) {
            unsigned a = coverage(r, radius, x, y);
            if (a)
                pixel(u, x, y, color, a);
        }
}
static void outline(vui_handle_t u, rect_t r, rect_t clip, unsigned radius, unsigned width,
                    uint16_t color)
{
    rect_t b = intersect(r, clip),
           inner = {r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width};
    for (int y = b.y; y < b.y + b.h; y++)
        for (int x = b.x; x < b.x + b.w; x++) {
            int a = (int)coverage(r, radius, x, y) -
                    (int)coverage(inner, max(0, (int)radius - (int)width), x, y);
            if (a > 0)
                pixel(u, x, y, color, a);
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
static void bitmap(vui_handle_t u, rect_t r, rect_t clip, const uint8_t *data, unsigned w,
                   unsigned h, bool rgb, size_t stride)
{
    if (!w || !h)
        return;
    rect_t b = intersect(r, clip);
    for (int y = b.y; y < b.y + b.h; y++)
        for (int x = b.x; x < b.x + b.w; x++) {
            unsigned sx = (x - r.x) * w / r.w, sy = (y - r.y) * h / r.h;
            const uint8_t *p = data + sy * stride + sx * (rgb ? 2 : 3);
            pixel(u, x, y, p[0] | (p[1] << 8), rgb ? 255 : p[2]);
        }
}
static void hit(vui_handle_t u, rect_t r, rect_t clip, unsigned node, vui_event_t event)
{
    r = intersect(r, clip);
    if (r.w > 0 && r.h > 0 && u->hit_count < HITS)
        u->hits[u->hit_count++] = (hit_t){r, node, event};
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
    if (n->flags & VUI_BACKGROUND)
        box(u, r, clip, n->radius, n->bg);
    if (n->border_width)
        outline(u, r, clip, n->radius, n->border_width, n->border);
    if (n->kind == VUI_PROGRESS)
        box(u, (rect_t){r.x, r.y, r.w * v->value / 100, r.h}, clip, n->radius, fg);
    if (n->kind == VUI_IMAGE) {
        if (v->canvas)
            bitmap(u, r, clip, v->canvas, n->w, n->h, true, v->stride);
        else if (v->image)
            bitmap(u, r, clip, v->image, v->width, v->height, false, v->width * 3);
        else if (n->image) {
            const vui_image_t *im = &u->images[n->image - 1];
            bitmap(u, r, clip,
                   u->assets + im->data + (u->now / im->duration % im->frames) * im->w * im->h * 3,
                   im->w, im->h, false, im->w * 3);
        }
    }
    if (n->kind == VUI_LABEL || n->kind == VUI_BUTTON)
        text(u, n, r, clip, v->text ? v->text : (const char *)u->assets + n->text, fg);
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
    if ((n->flags & VUI_SCROLL) && n->content_height > n->h) {
        int track = r.h - 8, thumb = max(20, track * r.h / n->content_height);
        int y = r.y + 4 + offset * (track - thumb) / (n->content_height - n->h);
        box(u, (rect_t){r.x + r.w - 12, r.y + 4, 4, track}, clip, 2, 0x2945);
        box(u, (rect_t){r.x + r.w - 12, y, 4, thumb}, clip, 2, 0x9cd3);
    }
}
bool vui_render(vui_handle_t u, uint16_t *pixels)
{
    if (!u || !pixels || !u->dirty)
        return false;
    u->pixels = pixels;
    u->dirty = false;
    u->hit_count = 0;
    memset(pixels, 0, WIDTH * HEIGHT * 2);
    for (unsigned i = 0; i < u->header->node_count; i++)
        if (u->nodes[i].parent < 0 && !(u->nodes[i].flags & VUI_TEMPLATE))
            draw(u, i, 0, 0, (rect_t){0, 0, WIDTH, HEIGHT}, NULL, 0);
    u->pixels = NULL;
    return true;
}
void vui_pointer(vui_handle_t u, int x, int y, bool pressed)
{
    if (!u)
        return;
    if (pressed && !u->pressed) {
        memset(&u->down, 0, sizeof(u->down));
        u->start_x = x;
        u->start_y = y;
        u->last_y = y;
        uint16_t coasting = u->fling_node;
        u->moved = false;
        u->fling_node = 0;
        u->velocity = u->scroll_fraction = 0;
        u->last_move = u->now;
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
            uint64_t elapsed_move = u->now - u->last_move;
            int dt = elapsed_move > 200 ? 200 : max(1, (int)elapsed_move);
            u->velocity = max(-3000, min(3000, (next - *position) * 1000 / dt));
            u->last_move = u->now;
            if (next != *position) { *position = next; u->dirty = true; }
        }
        u->last_y = y;
    }
    bool clicked =
        !pressed && u->pressed && !u->moved && u->down.node && contains(u->down.rect, x, y);
    if (!pressed && u->pressed && u->drag_node && u->moved &&
        u->now - u->last_move < 120 && abs(u->velocity) >= 20)
        u->fling_node = u->drag_node;
    if (u->observer)
        u->observer(u, x, y, pressed, u->observer_ctx);
    if (clicked && visible(u, u->down.node - 1)) {
        if (u->down.event.arg >= 0x100 && u->down.event.arg <= 0x102) {
            int layer = u->nodes[u->down.node - 1].parent, keyboard = u->nodes[layer].parent,
                mode = u->down.event.arg == 0x100   ? 1
                       : u->down.event.arg == 0x101 ? 0
                                                    : 2;
            for (int c = u->child[keyboard], m = 0; c >= 0; c = u->sibling[c])
                if (u->nodes[c].flags & VUI_KEY_MODE)
                    vui_set_visible(u, c + 1, m++ == mode);
        } else if (u->event)
            u->event(u, &u->down.event, u->event_ctx);
    }
    u->pressed = pressed;
}
