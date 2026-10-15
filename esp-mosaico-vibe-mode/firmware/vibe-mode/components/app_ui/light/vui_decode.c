// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "vui_memory.h"
#ifdef ESP_PLATFORM
#include "miniz.h"
bool vui_inflate(const void *input, size_t length, void *out, size_t size)
{
    tinfl_decompressor *d = vui_alloc(sizeof(*d));
    if (!d)
        return false;
    tinfl_init(d);
    size_t used = length, written = size;
    tinfl_status result =
        tinfl_decompress(d, input, &used, out, out, &written,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return result == TINFL_STATUS_DONE && used == length && written == size;
}
#else
#include <zlib.h>
bool vui_inflate(const void *input, size_t length, void *out, size_t size)
{
    uLongf written = size;
    uLong used = length;
    return uncompress2(out, &written, input, &used) == Z_OK && written == size && used == length;
}
#endif
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = ~0U;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
    }
    return ~c;
}
static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
/* PNG scanline/filter rules: https://www.w3.org/TR/png-3/#8Interlace */
static unsigned sample(const uint8_t *row, unsigned index, unsigned depth)
{
    if (depth == 16)
        return ((unsigned)row[index * 2] << 8) | row[index * 2 + 1];
    if (depth == 8)
        return row[index];
    return (row[index * depth / 8] >> (8 - depth - index * depth % 8)) & ((1U << depth) - 1);
}
static unsigned pass_extent(unsigned total, unsigned start, unsigned step)
{
    return total > start ? (total - start + step - 1) / step : 0;
}
uint8_t *vui_png(const uint8_t *in, size_t size, unsigned *width, unsigned *height)
{
    if (!in || size < 45 || size > 262144 || memcmp(in, "\211PNG\r\n\032\n", 8))
        return NULL;
    unsigned w = 0, h = 0, depth = 0, type = 0, channels = 0, colors = 0, interlace = 0;
    uint8_t palette[256][4];
    memset(palette, 255, sizeof(palette));
    uint8_t transparent[6] = {0};
    size_t trans = 0, used = 0;
    uint8_t *packed = vui_alloc(size), *raw = NULL, *pixels = NULL;
    bool end = false, header = false;
    if (!packed)
        return NULL;
    for (size_t pos = 8; pos + 12 <= size;) {
        uint32_t n = be32(in + pos);
        const uint8_t *tag = in + pos + 4, *data = in + pos + 8;
        if (n > size - pos - 12 || crc(tag, n + 4) != be32(data + n))
            goto done;
        if (!memcmp(tag, "IHDR", 4)) {
            if (header || pos != 8 || n != 13)
                goto done;
            header = true;
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            type = data[9];
            interlace = data[12];
            if (!w || !h || w > 256 || h > 256 || data[10] || data[11] || interlace > 1)
                goto done;
            channels = type == 0 || type == 3 ? 1
                       : type == 2            ? 3
                       : type == 4            ? 2
                       : type == 6            ? 4
                                              : 0;
            if (!channels ||
                !(depth == 8 || (depth == 16 && type != 3) ||
                  ((type == 0 || type == 3) && (depth == 1 || depth == 2 || depth == 4))))
                goto done;
        } else if (!header)
            goto done;
        else if (!memcmp(tag, "PLTE", 4)) {
            if (!n || n > 768 || n % 3)
                goto done;
            colors = n / 3;
            for (unsigned i = 0; i < colors; i++)
                memcpy(palette[i], data + i * 3, 3);
        } else if (!memcmp(tag, "tRNS", 4)) {
            if (type == 3) {
                if (n > colors)
                    goto done;
                for (unsigned i = 0; i < n; i++)
                    palette[i][3] = data[i];
            } else {
                if ((type == 0 && n != 2) || (type == 2 && n != 6) || (type != 0 && type != 2))
                    goto done;
                memcpy(transparent, data, n);
                trans = n;
            }
        } else if (!memcmp(tag, "IDAT", 4)) {
            memcpy(packed + used, data, n);
            used += n;
        } else if (!memcmp(tag, "IEND", 4)) {
            if (n || pos + 12 != size)
                goto done;
            end = true;
            break;
        } else if (!(tag[0] & 32))
            goto done; /* unknown critical chunk */
        pos += n + 12;
    }
    if (!end || !used || (type == 3 && !colors))
        goto done;
    /* A zero-sized Adam7 pass contains no filter bytes. */
    static const uint8_t passes[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                         {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    size_t total = 0;
    for (unsigned pass = 0; pass < (interlace ? 7U : 1U); pass++) {
        unsigned pw = interlace ? pass_extent(w, passes[pass][0], passes[pass][2]) : w;
        unsigned ph = interlace ? pass_extent(h, passes[pass][1], passes[pass][3]) : h;
        if (pw && ph)
            total += ((pw * channels * depth + 7) / 8 + 1) * ph;
    }
    raw = vui_alloc(total);
    pixels = vui_alloc(w * h * 3);
    if (!raw || !pixels || !vui_inflate(packed, used, raw, total))
        goto invalid_pixels;
    unsigned bpp = (channels * depth + 7) / 8;
    size_t offset = 0;
    for (unsigned pass = 0; pass < (interlace ? 7U : 1U); pass++) {
        unsigned x0 = interlace ? passes[pass][0] : 0, y0 = interlace ? passes[pass][1] : 0;
        unsigned dx = interlace ? passes[pass][2] : 1, dy = interlace ? passes[pass][3] : 1;
        unsigned pw = pass_extent(w, x0, dx), ph = pass_extent(h, y0, dy);
        if (!pw || !ph)
            continue;
        size_t stride = (pw * channels * depth + 7) / 8;
        for (unsigned y = 0; y < ph; y++) {
            uint8_t *row = raw + offset + 1, *prev = y ? row - stride - 1 : NULL;
            offset += stride + 1;
            unsigned filter = row[-1];
            if (filter > 4)
                goto invalid_pixels;
            for (unsigned x = 0; x < stride; x++) {
                unsigned a = x >= bpp ? row[x - bpp] : 0, b = prev ? prev[x] : 0,
                         c = prev && x >= bpp ? prev[x - bpp] : 0;
                row[x] += filter == 1   ? a
                          : filter == 2 ? b
                          : filter == 3 ? (a + b) / 2
                          : filter == 4 ? (unsigned)paeth(a, b, c)
                                        : 0;
            }
            for (unsigned x = 0; x < pw; x++) {
                unsigned v[4] = {0};
                for (unsigned ch = 0; ch < channels; ch++)
                    v[ch] = sample(row, x * channels + ch, depth);
                unsigned r, g, b, a = 255;
                if (type == 3) {
                    if (v[0] >= colors)
                        goto invalid_pixels;
                    r = palette[v[0]][0];
                    g = palette[v[0]][1];
                    b = palette[v[0]][2];
                    a = palette[v[0]][3];
                } else if (type == 0 || type == 4) {
                    r = g = b = depth == 16 ? v[0] >> 8 : v[0] * 255 / ((1U << depth) - 1);
                    if (type == 4)
                        a = depth == 16 ? v[1] >> 8 : v[1];
                    else if (trans && v[0] == ((unsigned)transparent[0] << 8 | transparent[1]))
                        a = 0;
                } else {
                    r = depth == 16 ? v[0] >> 8 : v[0];
                    g = depth == 16 ? v[1] >> 8 : v[1];
                    b = depth == 16 ? v[2] >> 8 : v[2];
                    if (type == 6)
                        a = depth == 16 ? v[3] >> 8 : v[3];
                    else if (trans && v[0] == ((unsigned)transparent[0] << 8 | transparent[1]) &&
                             v[1] == ((unsigned)transparent[2] << 8 | transparent[3]) &&
                             v[2] == ((unsigned)transparent[4] << 8 | transparent[5]))
                        a = 0;
                }
                unsigned c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3),
                         pos = ((y0 + y * dy) * w + x0 + x * dx) * 3;
                pixels[pos] = c;
                pixels[pos + 1] = c >> 8;
                pixels[pos + 2] = a;
            }
        }
    }
    *width = w;
    *height = h;
    goto done;
invalid_pixels:
    free(pixels);
    pixels = NULL;
done:
    free(packed);
    free(raw);
    return pixels;
}

/* Local title cache emitted by ideas_title_qoi: RGB/RUN opcodes only.
 * The black title surface is transparent over cards. Crop empty vertical
 * padding so the original 84px display block fits a single row label. */
uint8_t *vui_title_qoi(const uint8_t *in, size_t size, unsigned *width, unsigned *height)
{
    static const uint8_t end[8] = {0,0,0,0,0,0,0,1};
    if (!in || size < 23 || size > 14 + 328 * 84 * 4 + 8 || memcmp(in, "qoif", 4) ||
        be32(in + 4) != 328 || be32(in + 8) != 84 || in[12] != 3 || in[13] ||
        memcmp(in + size - 8, end, 8)) return NULL;
    const unsigned count = 328 * 84;
    uint8_t *out = vui_alloc(count * 3);
    if (!out) return NULL;
    size_t pos = 14;
    unsigned used = 0, first = 84, last = 0;
    uint16_t color = 0;
    while (pos < size - 8 && used < count) {
        unsigned tag = in[pos++], run = 1;
        if (tag == 0xfe) {
            if (size - 8 - pos < 3) goto invalid;
            color = ((in[pos] >> 3) << 11) | ((in[pos+1] >> 2) << 5) | (in[pos+2] >> 3);
            pos += 3;
        } else if (tag >= 0xc0 && tag <= 0xfd) run = (tag & 63) + 1;
        else goto invalid;
        if (run > count - used) goto invalid;
        while (run--) {
            out[used*3] = color;
            out[used*3+1] = color >> 8;
            out[used*3+2] = color ? 255 : 0;
            if (color) { if (first == 84) first = used / 328; last = used / 328; }
            ++used;
        }
    }
    if (used != count || pos != size - 8) goto invalid;
    if (first == 84) first = last = 0;
    *width = 328; *height = last - first + 1;
    memmove(out, out + first * 328 * 3, *height * 328 * 3);
    return out;
invalid:
    free(out);
    return NULL;
}
