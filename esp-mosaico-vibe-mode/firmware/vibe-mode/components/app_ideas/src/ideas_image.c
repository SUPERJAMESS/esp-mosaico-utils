// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "miniz.h"
#else
#include <zlib.h>
#endif

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t crc32_png(const unsigned char *p, size_t n) {
    uint32_t value = UINT32_MAX;
    while (n--) {
        value ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
bool ideas_png_valid(const void *data, size_t size) {
    static const unsigned char header[] = {0, 0, 0, 64, 0, 0, 0, 64, 8, 6, 0, 0, 0};
    const unsigned char *p = data;
    if (!p || size < 57 || size > 65536 || memcmp(p, "\x89PNG\r\n\x1a\n", 8) || be32(p + 8) != 13 ||
        memcmp(p + 12, "IHDR", 4) || memcmp(p + 16, header, sizeof(header)))
        return false;
    unsigned char *compressed = malloc(size), *pixels = malloc(64 * (64 * 4 + 1));
    if (!compressed || !pixels) {
        free(compressed);
        free(pixels);
        return false;
    }
    size_t offset = 8, used = 0;
    bool valid = false, ended = false, idat_ended = false;
    while (offset + 12 <= size) {
        uint32_t n = be32(p + offset);
        const unsigned char *tag = p + offset + 4;
        if (n > size - offset - 12 || crc32_png(tag, n + 4) != be32(tag + n + 4))
            goto done;
        if (!memcmp(tag, "acTL", 4) || !memcmp(tag, "fcTL", 4) || !memcmp(tag, "fdAT", 4))
            goto done;
        if (!memcmp(tag, "IHDR", 4)) {
            if (offset != 8 || n != 13)
                goto done;
        } else if (!memcmp(tag, "IDAT", 4)) {
            if (idat_ended)
                goto done;
            memcpy(compressed + used, tag + 4, n);
            used += n;
        } else {
            if (used)
                idat_ended = true;
            if (!memcmp(tag, "IEND", 4)) {
                if (n)
                    goto done;
                ended = true;
            } else if (!(tag[0] & 32) && memcmp(tag, "PLTE", 4))
                goto done;
        }
        offset += n + 12;
        if (ended)
            break;
    }
    if (!ended || offset != size || !used)
        goto done;
#ifdef ESP_PLATFORM
    tinfl_decompressor *decoder = malloc(sizeof(*decoder));
    if (!decoder)
        goto done;
    tinfl_init(decoder);
    size_t input = used, output = 64 * (64 * 4 + 1);
    tinfl_status result =
        tinfl_decompress(decoder, compressed, &input, pixels, pixels, &output,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decoder);
    if (result != TINFL_STATUS_DONE || input != used || output != 64 * (64 * 4 + 1))
        goto done;
#else
    uLong input = used, output = 64 * (64 * 4 + 1);
    if (uncompress2(pixels, &output, compressed, &input) != Z_OK || input != used ||
        output != 64 * (64 * 4 + 1))
        goto done;
#endif
    for (unsigned row = 0; row < 64; ++row)
        if (pixels[row * (64 * 4 + 1)] > 4)
            goto done;
    valid = true;
done:
    free(compressed);
    free(pixels);
    return valid;
}
