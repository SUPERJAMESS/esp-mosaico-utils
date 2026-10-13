// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *get(const cJSON *o, const char *key)
{ return cJSON_GetObjectItemCaseSensitive(o, key); }
static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = get(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool copy(const cJSON *o, const char *key, char *out, size_t size)
{
    const char *s = str(o, key);
    if (!*s || strlen(s) >= size) return false;
    memcpy(out, s, strlen(s) + 1);
    return true;
}
static bool number(const cJSON *o, const char *key, uint32_t *out)
{
    const cJSON *v = get(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < 0 || v->valuedouble > UINT32_MAX ||
        v->valuedouble != (uint32_t)v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}
static bool hash(const char *s)
{ return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
bool ideas_code_valid(const char *code)
{ return code && strlen(code) == 6 && strspn(code, "0123456789") == 6; }
bool ideas_filename_valid(const char *name)
{
    return name && *name && strlen(name) < 65 && name[0] != '.' &&
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(name) &&
        strcmp(name, "idea.json") && strcmp(name, "manifest.json");
}
static bool unique_keys(const cJSON *o, unsigned depth)
{
    if (depth > 12) return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, o) {
        if (cJSON_IsObject(o))
            for (const cJSON *p = v->next; p; p = p->next)
                if (!strcmp(v->string, p->string)) return false;
        if ((cJSON_IsObject(v) || cJSON_IsArray(v)) && !unique_keys(v, depth + 1)) return false;
    }
    return true;
}
static const ideas_file_t *file_for(const ideas_package_t *p, const char *name)
{
    for (size_t i = 0; i < p->file_count; i++)
        if (!strcmp(p->files[i].name, name)) return &p->files[i];
    return NULL;
}
static bool bounded_depth(const char *json, size_t size)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; i++) {
        char c = json[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 12) return false; }
        else if (c == ']' || c == '}') { if (!depth) return false; --depth; }
    }
    return !quoted && depth == 0;
}

esp_err_t ideas_package_parse(const char *json, size_t size, const char *code,
                             ideas_package_t *out)
{
    if (!json || !out || !ideas_code_valid(code) || !size || size > IDEAS_DESCRIPTOR_BYTES ||
        memchr(json, 0, size) || !bounded_depth(json, size)) return ESP_ERR_INVALID_ARG;
    /* cJSON stores strings as NUL-terminated values. Reject escaped NULs before
     * decoding so paths, identities and hashes cannot have hidden suffixes. */
    for (size_t i = 0; i + 6 <= size; i++)
        if (!memcmp(json + i, "\\u0000", 6)) return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!root) return ESP_ERR_INVALID_ARG;
    while (end < json + size && strchr(" \r\n\t", *end)) end++;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (end != json + size || !unique_keys(root, 0) ||
        strcmp(str(root, "schema"), "mosaico-ideas/download/1")) goto done;
    const cJSON *app = get(root, "application"), *manifest = get(root, "system_manifest");
    if (!copy(app, "code", out->code, sizeof(out->code)) || strcmp(out->code, code) ||
        !copy(app, "id", out->application_id, sizeof(out->application_id)) ||
        !copy(app, "release_id", out->release_id, sizeof(out->release_id)) ||
        !copy(app, "version", out->version, sizeof(out->version)) ||
        !copy(app, "name", out->name, sizeof(out->name)) ||
        !copy(app, "locale", out->locale, sizeof(out->locale))) goto done;
    const char *description = str(app, "description");
    if (strlen(description) >= sizeof(out->description)) goto done;
    strcpy(out->description, description);
    if (strcmp(str(manifest, "schema"), "esp-iris-system-update/0.2") ||
        strcmp(str(manifest, "release"), out->version) ||
        get(manifest, "signature") || get(manifest, "remote_bridge") ||
        get(manifest, "preserve_layout") || get(manifest, "source_layout_sha256") ||
        !copy(manifest, "target_layout_sha256", out->layout_sha256, sizeof(out->layout_sha256)) ||
        !hash(out->layout_sha256)) goto done;
    uint32_t chip;
    const cJSON *target = get(manifest, "target");
    if (!number(target, "chip_id", &chip) || chip != 32 ||
        !number(target, "flash_size", &out->flash_size) || !out->flash_size ||
        out->flash_size > IDEAS_MAX_PACKAGE_BYTES || out->flash_size % 4096) goto done;
    const cJSON *files = get(root, "files"), *components = get(manifest, "components");
    int count = cJSON_GetArraySize(files), components_n = cJSON_GetArraySize(components);
    if (!cJSON_IsArray(files) || count < 2 || count > IDEAS_MAX_FILES ||
        !cJSON_IsArray(components) || components_n < 2 || components_n > 8) goto done;
    out->file_count = count;
    out->component_count = components_n;
    for (int i = 0; i < count; i++) {
        const cJSON *f = cJSON_GetArrayItem(files, i);
        ideas_file_t *dest = &out->files[i];
        if (!copy(f, "name", dest->name, sizeof(dest->name)) || !ideas_filename_valid(dest->name) ||
            !copy(f, "path", dest->path, sizeof(dest->path)) ||
            strncmp(dest->path, "/api/v1/ideas/files/", 20) ||
            !ideas_filename_valid(dest->path + 20) ||
            !copy(f, "sha256", dest->sha256, sizeof(dest->sha256)) || !hash(dest->sha256) ||
            !number(f, "size", &dest->size) || !dest->size || dest->size > 16U * 1024U * 1024U)
            goto done;
        for (int j = 0; j < i; j++) if (!strcmp(dest->name, out->files[j].name)) goto done;
        out->total_size += dest->size;
    }
    if (out->total_size > IDEAS_MAX_PACKAGE_BYTES) goto done;
    bool used[IDEAS_MAX_FILES] = {false};
    uint32_t ids[8], offsets[8], sizes[8];
    unsigned applications = 0;
    for (int i = 0; i < components_n; i++) {
        const cJSON *c = cJSON_GetArrayItem(components, i);
        const char *kind = str(c, "kind"), *filename = str(c, "file");
        const ideas_file_t *f = file_for(out, filename);
        uint32_t flags = 0;
        if (!f || !number(c, "id", &ids[i]) || !ids[i] || ids[i] > 255 ||
            !number(c, "target_offset", &offsets[i]) || !number(c, "size", &sizes[i]) ||
            sizes[i] != f->size || strcmp(str(c, "sha256"), f->sha256) ||
            (get(c, "flags") && (!number(c, "flags", &flags) || flags)) ||
            (uint64_t)offsets[i] + sizes[i] > out->flash_size || offsets[i] % 4096 ||
            used[f - out->files]) goto done;
        used[f - out->files] = true;
        if (!strcmp(kind, "partition_table")) {
            if (i != 0 || offsets[i] != 0x8000 || sizes[i] != 4096 ||
                strcmp(f->sha256, out->layout_sha256)) goto done;
        } else if (i == 0) goto done;
        else if (!strcmp(kind, "application")) {
            if (++applications != 1 || offsets[i] != 0x210000) goto done;
            out->application_offset = offsets[i]; out->application_size = sizes[i];
            strcpy(out->application_sha256, f->sha256);
        } else if (strcmp(kind, "data") || offsets[i] < 0x210000) goto done;
        for (int j = 0; j < i; j++)
            if (ids[i] == ids[j] || ((uint64_t)offsets[i] < (uint64_t)offsets[j] + sizes[j] &&
                (uint64_t)offsets[j] < (uint64_t)offsets[i] + sizes[i])) goto done;
    }
    if (applications != 1) goto done;
    const char *assets[] = {"presentation", "icon"};
    char *names[] = {out->presentation, out->icon};
    for (unsigned i = 0; i < 2; i++) {
        const char *name = str(root, assets[i]);
        if (!*name) continue;
        const ideas_file_t *f = file_for(out, name);
        if (!f || used[f - out->files] || f->size > IDEAS_MAX_ASSET_BYTES) goto done;
        strcpy(names[i], name); used[f - out->files] = true;
    }
    const cJSON *fonts = get(root, "fonts");
    if (fonts && !cJSON_IsArray(fonts)) goto done;
    int font_count = cJSON_GetArraySize(fonts);
    if (font_count > IDEAS_MAX_FONTS || (font_count && !out->presentation[0]) ||
        (out->presentation[0] && !font_count)) goto done;
    out->font_count = font_count;
    uint64_t asset_bytes = 0;
    for (int i = 0; i < font_count; ++i) {
        const cJSON *name = cJSON_GetArrayItem(fonts, i);
        if (!cJSON_IsString(name)) goto done;
        const ideas_file_t *f = file_for(out, name->valuestring);
        if (!f || used[f - out->files] || f->size > IDEAS_MAX_ASSET_BYTES) goto done;
        strcpy(out->fonts[i], f->name); used[f - out->files] = true;
    }
    for (int i = 0; i < count; ++i) {
        if (!used[i]) goto done;
        bool asset = !strcmp(out->presentation, out->files[i].name) || !strcmp(out->icon, out->files[i].name);
        for (int j = 0; j < font_count; ++j) asset |= !strcmp(out->fonts[j], out->files[i].name);
        if (asset) asset_bytes += out->files[i].size;
    }
    if (asset_bytes > IDEAS_MAX_ASSET_BYTES) goto done;
    const ideas_file_t *icon = file_for(out, out->icon), *display = file_for(out, out->presentation);
    if (icon && icon->size > 65536) goto done;
    if (display && display->size > 32768) goto done;
    char *encoded = cJSON_PrintUnformatted(manifest);
    if (!encoded) { err = ESP_ERR_NO_MEM; goto done; }
    if (strlen(encoded) <= IDEAS_MANIFEST_BYTES) {
        strcpy(out->manifest, encoded); err = ESP_OK;
    }
    cJSON_free(encoded);
done:
    cJSON_Delete(root);
    return err;
}
