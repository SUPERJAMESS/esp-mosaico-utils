// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ideas_internal.h"

#define IDEAS_PREVIEW_BYTES (65536U + 3072U + 1048576U + 4096U)
typedef struct {
    pending_t *pending;
    const char *code;
    unsigned language, phase, part, flags;
    char boundary[71], frame[80], line[256], path[IDEAS_PATH_BYTES];
    size_t frame_used, line_used, framing, length, used;
    bool json;
    uint8_t *target;
} ideas_multipart_t;

esp_err_t ideas_multipart_begin(ideas_multipart_t *s, pending_t *pending, const char *code,
                                unsigned language, const char *media);
esp_err_t ideas_multipart_feed(ideas_multipart_t *s, const void *data, size_t size);
bool ideas_multipart_complete(const ideas_multipart_t *s);
