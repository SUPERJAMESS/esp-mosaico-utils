// SPDX-License-Identifier: Apache-2.0
#include "ideas_presentation.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

static const cJSON *get(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static const char *string(const cJSON *o, const char *key)
{
    const cJSON *v = get(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool equal(const cJSON *o, const char *key, const char *expected)
{
    const char *v = string(o, key);
    return v && expected && !strcmp(v, expected);
}
static bool keys(const cJSON *o, const char *const *allowed, size_t count)
{
    if (!cJSON_IsObject(o) || (size_t)cJSON_GetArraySize(o) != count)
        return false;
    for (const cJSON *v = o->child; v; v = v->next) {
        bool known = false;
        for (size_t i = 0; i < count; ++i)
            known |= !strcmp(v->string, allowed[i]);
        if (!known)
            return false;
        for (const cJSON *next = v->next; next; next = next->next)
            if (!strcmp(v->string, next->string))
                return false;
    }
    return true;
}
static bool integer(const cJSON *v, int64_t low, int64_t high, int64_t *out)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
        v->valuedouble > high || v->valuedouble != (int64_t)v->valuedouble)
        return false;
    *out = (int64_t)v->valuedouble;
    return true;
}
static bool bounded(const char *data, size_t size)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        char c = data[i];
        if (!c || (i + 6 <= size && !memcmp(data + i, "\\u0000", 6)))
            return false;
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{') {
            if (++depth > 8)
                return false;
        } else if (c == ']' || c == '}') {
            if (!depth)
                return false;
            --depth;
        }
    }
    return !quoted && !depth;
}
bool ideas_presentation_parse(const void *json, size_t size, const ideas_display_sources_t *sources,
                              ideas_presentation_t *out)
{
    if (!json || !sources || !out || !size || size > IDEAS_DISPLAY_JSON_BYTES ||
        !sources->font_count || sources->font_count > IDEAS_DISPLAY_FONTS || !bounded(json, size))
        return false;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sources->font_count; ++i) {
        if (!sources->fonts[i].name ||
            !ideas_font_open(sources->fonts[i].data, sources->fonts[i].size, &out->fonts[i]))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(sources->fonts[i].name, sources->fonts[j].name))
                return false;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!root)
        return false;
    while (end < (const char *)json + size && strchr(" \r\n\t", *end))
        ++end;
    bool valid = false, used[IDEAS_DISPLAY_FONTS] = {false};
    static const char *const root_keys[] = {"schema", "locale", "icon", "name", "description"};
    static const char *const text_keys[] = {"text", "font", "width", "height", "glyphs"};
    if (end != (const char *)json + size || !keys(root, root_keys, 5) ||
        !equal(root, "schema", "mosaico-ideas/display/1") ||
        !equal(root, "locale", sources->locale) || !equal(root, "icon", sources->icon))
        goto done;
    for (unsigned i = 0; i < 2; ++i) {
        const cJSON *text = get(root, i ? "description" : "name");
        if (!keys(text, text_keys, 5) ||
            !equal(text, "text", i ? sources->description : sources->name))
            goto done;
        ideas_text_t *dest = &out->text[i];
        int64_t w, h;
        if (!integer(get(text, "width"), 1, i ? IDEAS_DESCRIPTION_WIDTH : IDEAS_TITLE_WIDTH, &w) ||
            !integer(get(text, "height"), 1, i ? IDEAS_DESCRIPTION_HEIGHT : IDEAS_TITLE_HEIGHT, &h))
            goto done;
        dest->width = w;
        dest->height = h;
        const char *font = string(text, "font");
        size_t index = 0;
        while (index < sources->font_count && (!font || strcmp(font, sources->fonts[index].name)))
            ++index;
        if (index == sources->font_count)
            goto done;
        used[index] = true;
        dest->font = index;
        const cJSON *glyphs = get(text, "glyphs");
        int count = cJSON_GetArraySize(glyphs);
        if (!cJSON_IsArray(glyphs) || count > (int)IDEAS_DISPLAY_RUNS ||
            (!count && *(i ? sources->description : sources->name)))
            goto done;
        dest->count = count;
        for (int n = 0; n < count; ++n) {
            const cJSON *entry = cJSON_GetArrayItem(glyphs, n);
            int64_t id, x, y;
            if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 3 ||
                !integer(cJSON_GetArrayItem(entry, 0), 1, UINT32_MAX, &id) ||
                !integer(cJSON_GetArrayItem(entry, 1), -192, w + 192, &x) ||
                !integer(cJSON_GetArrayItem(entry, 2), -192, h + 192, &y))
                goto done;
            ideas_glyph_t glyph;
            if (!ideas_font_glyph(&out->fonts[index], id, &glyph))
                goto done;
            if (glyph.width && (x + glyph.left < 0 || x + glyph.left + glyph.width > w ||
                                y - glyph.top < 0 || y - glyph.top + glyph.height > h))
                goto done;
            dest->glyphs[n] = (ideas_glyph_position_t){id, x, y};
        }
    }
    for (size_t i = 0; i < sources->font_count; ++i)
        if (!used[i])
            goto done;
    valid = true;
done:
    cJSON_Delete(root);
    return valid;
}
static uint16_t blend(uint16_t foreground, uint16_t background, unsigned alpha)
{
    unsigned r =
        (((foreground >> 11) & 31) * alpha + ((background >> 11) & 31) * (255 - alpha) + 127) / 255;
    unsigned g =
        (((foreground >> 5) & 63) * alpha + ((background >> 5) & 63) * (255 - alpha) + 127) / 255;
    unsigned b = ((foreground & 31) * alpha + (background & 31) * (255 - alpha) + 127) / 255;
    return (r << 11) | (g << 5) | b;
}
bool ideas_presentation_render(const ideas_presentation_t *display, unsigned block,
                               uint16_t foreground, uint16_t background, uint16_t *pixels,
                               size_t count)
{
    if (!display || block > 1 || !pixels)
        return false;
    unsigned width = block ? IDEAS_DESCRIPTION_WIDTH : IDEAS_TITLE_WIDTH;
    unsigned height = block ? IDEAS_DESCRIPTION_HEIGHT : IDEAS_TITLE_HEIGHT;
    if (count != (size_t)width * height)
        return false;
    for (size_t i = 0; i < count; ++i)
        pixels[i] = background;
    const ideas_text_t *text = &display->text[block];
    for (unsigned i = 0; i < text->count; ++i) {
        const ideas_glyph_position_t *p = &text->glyphs[i];
        ideas_glyph_t glyph;
        if (!ideas_font_glyph(&display->fonts[text->font], p->glyph, &glyph))
            return false;
        for (unsigned y = 0; y < glyph.height; ++y)
            for (unsigned x = 0; x < glyph.width; ++x) {
                size_t offset = (p->y - glyph.top + y) * width + p->x + glyph.left + x;
                pixels[offset] =
                    blend(foreground, pixels[offset], glyph.alpha[y * glyph.width + x]);
            }
    }
    return true;
}

bool ideas_presentation_title(const ideas_presentation_t *display, uint16_t foreground,
                              uint16_t background, uint16_t *pixels, size_t capacity,
                              unsigned *width, unsigned *height)
{
    if (!display || !pixels || !width || !height) return false;
    const ideas_text_t *text = &display->text[0];
    if (!text->count || text->font >= IDEAS_DISPLAY_FONTS) return false;
    const ideas_font_t *font = &display->fonts[text->font];
    if (!font->line_height) return false;
    /* display/1 keeps the publisher's shaped positions. Translate whole lines,
     * preserving kerning, ligatures and combining-mark offsets within a line.
     * Full-width glyphs need no word separator at a soft line break. */
    typedef struct { int key, left, right, start, first_advance, last_advance; } line_t;
    line_t lines[IDEAS_TITLE_HEIGHT + 1] = {0};
    unsigned line_count = 0;
    int baseline = text->glyphs[0].y, top = INT16_MAX, bottom = INT16_MIN;
    for (unsigned i = 0; i < text->count; ++i) {
        const ideas_glyph_position_t *p = &text->glyphs[i];
        ideas_glyph_t g;
        if (!ideas_font_glyph(font, p->glyph, &g)) return false;
        int delta = p->y - baseline, step = font->line_height;
        int key = delta >= 0 ? (delta + step / 2) / step : -((-delta + step / 2) / step);
        unsigned l = 0;
        while (l < line_count && lines[l].key != key) ++l;
        if (l == line_count) {
            if (line_count == IDEAS_TITLE_HEIGHT + 1) return false;
            lines[l] = (line_t){.key=key, .left=INT16_MAX, .right=INT16_MIN,
                                .first_advance=g.advance_q6};
            ++line_count;
        }
        line_t *line = &lines[l];
        int left = p->x + g.left, right = left + g.width;
        int advance = p->x + (g.advance_q6 + 63) / 64;
        if (advance > right) right = advance;
        if (left < line->left) line->left = left;
        if (right > line->right) line->right = right;
        line->last_advance = g.advance_q6;
        if (g.width && g.height) {
            int y = p->y - key * step - g.top;
            if (y < top) top = y;
            if (y + g.height > bottom) bottom = y + g.height;
        }
    }
    if (top == INT16_MAX) top = 0, bottom = 1;
    unsigned w = 0, h = (unsigned)(bottom - top);
    for (unsigned l = 0; l < line_count; ++l) {
        if (l && (lines[l-1].last_advance < font->pixels * 58 ||
                  lines[l].first_advance < font->pixels * 58))
            w += font->pixels / 3;
        lines[l].start = (int)w - lines[l].left;
        if (lines[l].right < lines[l].left) return false;
        w += (unsigned)(lines[l].right - lines[l].left);
    }
    if (!w) w = 1;
    if (w > IDEAS_TITLE_LINE_MAX_WIDTH || !h || h > IDEAS_TITLE_HEIGHT ||
        (size_t)w * h > capacity || (size_t)w * h > IDEAS_TITLE_PIXELS) return false;
    for (size_t i = 0; i < (size_t)w * h; ++i) pixels[i] = background;
    for (unsigned i = 0; i < text->count; ++i) {
        const ideas_glyph_position_t *p = &text->glyphs[i];
        ideas_glyph_t g;
        if (!ideas_font_glyph(font, p->glyph, &g)) return false;
        int delta = p->y - baseline, step = font->line_height;
        int key = delta >= 0 ? (delta + step / 2) / step : -((-delta + step / 2) / step);
        unsigned l = 0;
        while (l < line_count && lines[l].key != key) ++l;
        if (l == line_count) return false;
        int x0 = p->x + g.left + lines[l].start;
        int y0 = p->y - key * step - g.top - top;
        if (g.width && g.height && (x0 < 0 || y0 < 0 || x0 + g.width > (int)w || y0 + g.height > (int)h))
            return false;
        for (unsigned y = 0; y < g.height; ++y)
            for (unsigned x = 0; x < g.width; ++x) {
                size_t at = (y0 + y) * w + x0 + x;
                pixels[at] = blend(foreground, pixels[at], g.alpha[y * g.width + x]);
            }
    }
    *width = w; *height = h;
    return true;
}
