// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static inline void *vui_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void *vui_calloc(size_t n, size_t s)
{
    return heap_caps_calloc(n, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
#else
#define vui_alloc malloc
#define vui_calloc calloc
#endif
bool vui_inflate(const void *input, size_t length, void *out, size_t size);
/* Decoded pixels are packed little-endian RGB565 + coverage byte. */
uint8_t *vui_png(const uint8_t *input, size_t size, unsigned *w, unsigned *h);
