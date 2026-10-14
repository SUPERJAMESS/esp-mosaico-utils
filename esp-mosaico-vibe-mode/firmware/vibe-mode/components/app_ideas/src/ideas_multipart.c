// SPDX-License-Identifier: Apache-2.0
#include "ideas_multipart.h"
#include <ctype.h>

enum { FRAME, HEADERS, BODY, DONE };

/* Restricted MIME profile: two parameters, no preamble, nested parts, transfer
 * encoding or epilogue. Header names/parameter order are not significant. */
static bool boundary(const char *media, char out[71]) {
    if (strncasecmp(media, "multipart/related", 17))
        return false;
    const char *p = media + 17;
    unsigned seen = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        if (*p++ != ';')
            return false;
        while (*p == ' ' || *p == '\t')
            ++p;
        char key[16], value[80];
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '-') {
            if (n + 1 >= sizeof(key))
                return false;
            key[n++] = *p++;
        }
        key[n] = 0;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p++ != '=')
            return false;
        while (*p == ' ' || *p == '\t')
            ++p;
        bool quoted = *p == '"';
        if (quoted)
            ++p;
        n = 0;
        while (*p && (quoted ? *p != '"' : *p != ';' && *p != ' ' && *p != '\t')) {
            if (n + 1 >= sizeof(value))
                return false;
            value[n++] = *p++;
        }
        value[n] = 0;
        if (quoted && *p++ != '"')
            return false;
        unsigned bit;
        if (!strcasecmp(key, "type")) {
            if (strcasecmp(value, "application/json"))
                return false;
            bit = 1;
        } else if (!strcasecmp(key, "boundary")) {
            if (!n || n > 70)
                return false;
            for (size_t i = 0; i < n; ++i)
                if (!isalnum((unsigned char)value[i]) && value[i] != '-' && value[i] != '_')
                    return false;
            strcpy(out, value);
            bit = 2;
        } else
            return false;
        if (seen & bit)
            return false;
        seen |= bit;
    }
    return seen == 3;
}

esp_err_t ideas_multipart_begin(ideas_multipart_t *s, pending_t *p, const char *code,
                                unsigned language, const char *media) {
    /* A retry starts from an empty collection, including a previously parsed
     * root. No bytes or metadata may leak across HTTP attempts. */
    free(p->json);
    p->json = NULL;
    p->json_size = 0;
    for (unsigned i = 0; i < IDEAS_MAX_FILES; ++i) {
        free(p->assets[i].data);
        p->assets[i] = (icon_t){0};
    }
    memset(&p->package, 0, sizeof(p->package));
    memset(s, 0, sizeof(*s));
    s->pending = p;
    s->code = code;
    s->language = language;
    if (language > 1 || !boundary(media, s->boundary))
        return ESP_ERR_INVALID_ARG;
    snprintf(s->frame, sizeof(s->frame), "--%s\r\n", s->boundary);
    return ESP_OK;
}

static unsigned index_of(const ideas_multipart_t *s) {
    const ideas_package_t *p = &s->pending->package;
    return s->part == 1   ? 0
           : s->part == 2 ? p->display_index[s->language]
                          : p->asset_index[s->language][s->part - 3];
}

static esp_err_t open_part(ideas_multipart_t *s) {
    if (s->flags != 7 || !s->length)
        return ESP_ERR_INVALID_ARG;
    pending_t *p = s->pending;
    const char *expected = "manifest.json";
    if (!s->part) {
        if (s->length > IDEAS_DESCRIPTOR_BYTES)
            return ESP_ERR_INVALID_SIZE;
    } else {
        const ideas_file_t *file = &p->package.files[index_of(s)];
        expected = file->name;
        if (s->length != file->size)
            return ESP_ERR_INVALID_SIZE;
    }
    bool json = s->part < 3;
    if (strcmp(s->path, expected) || s->json != json)
        return ESP_ERR_INVALID_ARG;
    s->target = heap_caps_malloc(s->length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->target)
        return ESP_ERR_NO_MEM;
    if (!s->part)
        p->json = (char *)s->target;
    else
        p->assets[index_of(s)].data = s->target;
    s->used = 0;
    s->phase = BODY;
    return ESP_OK;
}

static esp_err_t header(ideas_multipart_t *s) {
    char *colon = strchr(s->line, ':');
    if (!colon)
        return ESP_ERR_INVALID_ARG;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        ++colon;
    char *end = colon + strlen(colon);
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;
    unsigned bit;
    if (!strcasecmp(s->line, "Content-Length")) {
        bit = 1;
        size_t n = 0;
        if (!*colon)
            return ESP_ERR_INVALID_ARG;
        for (const char *p = colon; *p; ++p) {
            if (*p < '0' || *p > '9' || n > IDEAS_PREVIEW_BYTES / 10)
                return ESP_ERR_INVALID_SIZE;
            n = n * 10 + *p - '0';
        }
        if (n > IDEAS_PREVIEW_BYTES)
            return ESP_ERR_INVALID_SIZE;
        s->length = n;
    } else if (!strcasecmp(s->line, "Content-Location")) {
        bit = 2;
        if (strlen(colon) >= sizeof(s->path))
            return ESP_ERR_INVALID_SIZE;
        strcpy(s->path, colon);
    } else if (!strcasecmp(s->line, "Content-Type")) {
        bit = 4;
        s->json = !strcasecmp(colon, "application/json");
        if (!s->json && strcasecmp(colon, "application/octet-stream"))
            return ESP_ERR_INVALID_ARG;
    } else
        return ESP_ERR_INVALID_ARG;
    if (s->flags & bit)
        return ESP_ERR_INVALID_ARG;
    s->flags |= bit;
    return ESP_OK;
}

static esp_err_t finish_part(ideas_multipart_t *s) {
    pending_t *p = s->pending;
    s->target[s->length] = 0;
    esp_err_t err = ESP_OK;
    if (!s->part) {
        p->json_size = s->length;
        err = ideas_package_parse(p->json, p->json_size, s->code, &p->package);
    } else {
        unsigned index = index_of(s);
        char sha[65];
        if (!ideas_hash_data(s->target, s->length, sha) ||
            strcmp(sha, p->package.files[index].sha256))
            return ESP_ERR_INVALID_CRC;
        p->assets[index].size = s->length;
        if (s->part == 1)
            err = ideas_package_firmware(&p->package, (char *)s->target, s->length);
    }
    if (err != ESP_OK)
        return err;
    ++s->part;
    bool last = s->part == 3 + p->package.asset_count[s->language];
    snprintf(s->frame, sizeof(s->frame), "\r\n--%s%s\r\n", s->boundary, last ? "--" : "");
    s->frame_used = 0;
    s->phase = FRAME;
    return ESP_OK;
}

esp_err_t ideas_multipart_feed(ideas_multipart_t *s, const void *data, size_t size) {
    const uint8_t *bytes = data;
    while (size) {
        if (s->phase == DONE)
            return ESP_ERR_INVALID_ARG;
        if (s->phase == BODY) {
            size_t n = s->length - s->used;
            if (n > size)
                n = size;
            memcpy(s->target + s->used, bytes, n);
            s->used += n;
            bytes += n;
            size -= n;
            if (s->used == s->length) {
                esp_err_t err = finish_part(s);
                if (err != ESP_OK)
                    return err;
            }
            continue;
        }
        if (++s->framing > 4096)
            return ESP_ERR_INVALID_SIZE;
        unsigned char c = *bytes++;
        --size;
        if (s->phase == FRAME) {
            if (c != (unsigned char)s->frame[s->frame_used++])
                return ESP_ERR_INVALID_ARG;
            if (!s->frame[s->frame_used]) {
                s->phase = s->part && s->part == 3 + s->pending->package.asset_count[s->language]
                               ? DONE
                               : HEADERS;
                s->line_used = s->flags = 0;
            }
        } else {
            if (!c || c > 127 || s->line_used + 1 >= sizeof(s->line))
                return ESP_ERR_INVALID_SIZE;
            s->line[s->line_used++] = c;
            if (c == '\n') {
                if (s->line_used < 2 || s->line[s->line_used - 2] != '\r')
                    return ESP_ERR_INVALID_ARG;
                s->line[s->line_used - 2] = 0;
                esp_err_t err = s->line_used == 2 ? open_part(s) : header(s);
                s->line_used = 0;
                if (err != ESP_OK)
                    return err;
            }
        }
    }
    return ESP_OK;
}

bool ideas_multipart_complete(const ideas_multipart_t *s) { return s->phase == DONE; }
