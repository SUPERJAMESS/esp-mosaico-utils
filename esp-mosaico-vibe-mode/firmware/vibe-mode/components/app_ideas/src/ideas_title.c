// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "ideas_presentation.h"
#include <stdlib.h>
#include <string.h>
/* QOI is a local rendering cache, never another downloaded protocol asset.
 * The row consumes the same display.name glyph block as the detail screen. */
void *ideas_title_qoi(const uint16_t *pixels, size_t *size) {
    const unsigned count = IDEAS_TITLE_WIDTH * IDEAS_TITLE_HEIGHT;
    if (!pixels || !size)
        return NULL;
    unsigned char *out = malloc(14 + count * 4 + 8);
    if (!out)
        return NULL;
    memcpy(out, "qoif\0\0\1\x48\0\0\0\x54\3\0", 14);
    unsigned previous = 0, run = 0;
    size_t used = 14;
    for (unsigned i = 0; i < count; ++i) {
        unsigned value = pixels[i];
        if (value == previous) {
            if (++run == 62 || i == count - 1) {
                out[used++] = 0xc0 | (run - 1);
                run = 0;
            }
        } else {
            if (run) {
                out[used++] = 0xc0 | (run - 1);
                run = 0;
            }
            out[used++] = 0xfe;
            unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
            out[used++] = (r << 3) | (r >> 2);
            out[used++] = (g << 2) | (g >> 4);
            out[used++] = (b << 3) | (b >> 2);
            previous = value;
        }
    }
    memset(out + used, 0, 8);
    out[used + 7] = 1;
    used += 8;
    void *small = realloc(out, used);
    *size = used;
    return small ? small : out;
}
