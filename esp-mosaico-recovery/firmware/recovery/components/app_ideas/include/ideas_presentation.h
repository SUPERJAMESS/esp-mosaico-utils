// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ideas_font.h"

/* Display/1 describes fixed text blocks, never scenes, widgets or actions.
 * Coordinates are local to each block; the UI adapter owns screen placement. */
#define IDEAS_DISPLAY_JSON_BYTES 32768U
#define IDEAS_DISPLAY_FONTS 2U
#define IDEAS_DISPLAY_RUNS 512U
#define IDEAS_TITLE_WIDTH 328U
#define IDEAS_TITLE_HEIGHT 84U
#define IDEAS_DESCRIPTION_WIDTH 416U
#define IDEAS_DESCRIPTION_HEIGHT 176U

typedef struct {
    const char *name;
    const void *data;
    size_t size;
} ideas_font_source_t;
typedef struct {
    const char *name, *description, *locale, *icon;
    size_t font_count;
    ideas_font_source_t fonts[IDEAS_DISPLAY_FONTS];
} ideas_display_sources_t;
typedef struct {
    uint32_t glyph;
    int16_t x, y;
} ideas_glyph_position_t;
typedef struct {
    uint16_t width, height, count, font;
    ideas_glyph_position_t glyphs[IDEAS_DISPLAY_RUNS];
} ideas_text_t;
typedef struct {
    ideas_font_t fonts[IDEAS_DISPLAY_FONTS];
    ideas_text_t text[2]; /* name, description */
} ideas_presentation_t;

/* All font bytes are borrowed. Validates references, metadata and every glyph
 * extent before the caller may publish a ready preview. No UI/OS dependency.
 * Parse size bytes of display JSON against borrowed sources into out.
 * Returns false on any invalid reference/extent; source bytes must outlive out. */
bool ideas_presentation_parse(const void *json, size_t size, const ideas_display_sources_t *sources,
                              ideas_presentation_t *out);
/* Opaque RGB565, native uint16_t pixels. Caller supplies the complete block.
 * Foreground/background are adapter choices, not remote UI instructions.
 * Render block (0 title, 1 body) into count caller-owned RGB565 pixels.
 * foreground/background are local UI colors; false means invalid block/buffer. */
bool ideas_presentation_render(const ideas_presentation_t *display, unsigned block,
                               uint16_t foreground, uint16_t background, uint16_t *pixels,
                               size_t count);
