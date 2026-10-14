// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MIFF/1: a bounded, little-endian A8 bitmap font subset. No UI/SDK types.
 * Glyph IDs identify shaped font glyphs, not Unicode code points. The caller
 * owns the bytes for the lifetime of the font and any returned glyph views. */
#define IDEAS_FONT_MAX_BYTES (1024U * 1024U)
#define IDEAS_FONT_MAX_GLYPHS 512U
typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t count, pixels, line_height;
} ideas_font_t;
typedef struct {
    const uint8_t *alpha;
    uint16_t width, height;
    int16_t left, top;
    int32_t advance_q6;
} ideas_glyph_t;

/* Validate size bytes of borrowed MIFF data and fill required out.
 * Returns false for malformed input; data must outlive out and glyph views. */
bool ideas_font_open(const void *data, size_t size, ideas_font_t *out);
/* Find shaped glyph id in font and copy its borrowed view into out.
 * Returns false for a missing or invalid glyph; performs no allocation. */
bool ideas_font_glyph(const ideas_font_t *font, uint32_t id, ideas_glyph_t *out);
