// SPDX-License-Identifier: Apache-2.0
#include "ideas_font.h"
#include <string.h>

#define HEADER_BYTES 24U
#define GLYPH_BYTES 20U
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p)
{ return u16(p) | (uint32_t)u16(p + 2) << 16; }
static int16_t i16(const uint8_t *p)
{ uint16_t n = u16(p); return n <= INT16_MAX ? (int16_t)n : (int16_t)(-1 - (UINT16_MAX - n)); }
static int32_t i32(const uint8_t *p)
{ uint32_t n = u32(p); return n <= INT32_MAX ? (int32_t)n : -1 - (int32_t)(UINT32_MAX - n); }

bool ideas_font_open(const void *data, size_t size, ideas_font_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || size < HEADER_BYTES || size > IDEAS_FONT_MAX_BYTES) return false;
    const uint8_t *p = data;
    uint16_t count = u16(p + 12), pixels = u16(p + 14), line = u16(p + 16);
    if (memcmp(p, "MIFF", 4) || u16(p + 4) != 1 || u16(p + 6) != HEADER_BYTES ||
        u32(p + 8) != size || !count || count > IDEAS_FONT_MAX_GLYPHS ||
        !pixels || pixels > 96 || !line || line > 192 || u16(p + 18) ||
        u32(p + 20) != HEADER_BYTES) return false;
    size_t end = HEADER_BYTES + (size_t)count * GLYPH_BYTES;
    if (end > size) return false;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *g = p + HEADER_BYTES + i * GLYPH_BYTES;
        uint32_t id = u32(g), offset = u32(g + 4);
        uint16_t w = u16(g + 8), h = u16(g + 10);
        int16_t left = i16(g + 12), top = i16(g + 14);
        int32_t advance = i32(g + 16);
        size_t length = (size_t)w * h;
        if (!id || id <= previous || offset != end || w > 192 || h > 192 ||
            (!w != !h) || left < -192 || left > 192 || top < -192 || top > 192 ||
            advance < -192 * 64 || advance > 192 * 64 || length > size - end) return false;
        end += length; previous = id;
    }
    if (end != size) return false;
    *out = (ideas_font_t){.data = p, .size = size, .count = count,
                          .pixels = pixels, .line_height = line};
    return true;
}
bool ideas_font_glyph(const ideas_font_t *font, uint32_t id, ideas_glyph_t *out)
{
    if (!font || !font->data || !out) return false;
    size_t lo = 0, hi = font->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *g = font->data + HEADER_BYTES + mid * GLYPH_BYTES;
        uint32_t found = u32(g);
        if (found < id) lo = mid + 1;
        else if (found > id) hi = mid;
        else {
            *out = (ideas_glyph_t){.alpha = font->data + u32(g + 4),
                .width = u16(g + 8), .height = u16(g + 10),
                .left = i16(g + 12), .top = i16(g + 14), .advance_q6 = i32(g + 16)};
            return true;
        }
    }
    return false;
}
