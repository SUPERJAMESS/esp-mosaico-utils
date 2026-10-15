// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
/* All integers in the generated immutable asset file are little endian.
 * Structures contain no pointers; the same file is used on the S31 and PC. */
typedef struct {
    uint32_t magic, size, nodes, node_count, fonts, font_count, images, image_count;
} vui_asset_header_t;
typedef struct {
    int16_t parent, x, y, w, h;
    uint16_t kind, flags, radius, bg, fg, border, border_width, font, align;
    uint16_t action, arg, image, template_node, item_height, content_height;
    uint32_t text;
} vui_node_t;
typedef struct {
    uint32_t glyphs, count;
    int16_t ascent, descent, height, reserved;
} vui_font_t;
typedef struct {
    uint32_t code, data;
    int16_t x, y, w, h, advance, reserved;
} vui_glyph_t;
typedef struct {
    uint32_t data;
    uint16_t w, h, frames, duration;
} vui_image_t;
enum {
    VUI_LAYER,
    VUI_CONTAINER,
    VUI_LABEL,
    VUI_BUTTON,
    VUI_IMAGE,
    VUI_LIST,
    VUI_PROGRESS,
    VUI_KEYBOARD,
    VUI_DOTS
};
enum { VUI_HIDDEN = 1, VUI_TEMPLATE = 2, VUI_BACKGROUND = 4, VUI_ELLIPSIS = 8, VUI_KEY_MODE = 16, VUI_SCROLL = 32 };
