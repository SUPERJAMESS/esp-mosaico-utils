// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const cJSON *get(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = get(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool copy(const cJSON *o, const char *key, char *out, size_t size) {
    const cJSON *v = get(o, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size)
        return false;
    strcpy(out, v->valuestring);
    return true;
}
static bool number(const cJSON *o, const char *key, uint32_t *out) {
    const cJSON *v = get(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || v->valuedouble != (uint32_t)v->valuedouble)
        return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}
static bool hash(const char *s) { return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
static bool alnum_ascii(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool ideas_code_valid(const char *code) {
    return code && strlen(code) == 6 && strspn(code, "0123456789") == 6;
}
bool ideas_local_code_valid(const char *code) {
    return ideas_code_valid(code) || (code && strlen(code) == 6 && code[0] == 'd' &&
                                      strspn(code + 1, "0123456789abcdef") == 5);
}
bool ideas_path_valid(const char *path) {
    if (!path || !*path || strlen(path) > 192)
        return false;
    unsigned segments = 1, length = 0;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') {
            if (!length || ++segments > 4)
                return false;
            length = 0;
        } else {
            if ((!length && !alnum_ascii(*p)) || (!alnum_ascii(*p) && !strchr("._-", *p)) ||
                ++length > 64)
                return false;
        }
    }
    return length != 0;
}
bool ideas_filename_valid(const char *name) {
    return ideas_path_valid(name) && !strchr(name, '/') && strcasecmp(name, "manifest.json");
}
static bool identity(const char *value) { return ideas_path_valid(value) && !strchr(value, '/'); }
static bool keys(const cJSON *o, const char *allowed, unsigned minimum, unsigned maximum) {
    if (!cJSON_IsObject(o))
        return false;
    unsigned count = 0;
    for (const cJSON *v = o->child; v; v = v->next) {
        char key[80];
        if (!v->string || snprintf(key, sizeof(key), "|%s|", v->string) >= sizeof(key) ||
            !strstr(allowed, key))
            return false;
        ++count;
    }
    return count >= minimum && count <= maximum;
}
static bool unique(const cJSON *o, unsigned depth) {
    if (depth > 12)
        return false;
    for (const cJSON *v = o->child; v; v = v->next) {
        if (cJSON_IsObject(o))
            for (const cJSON *p = v->next; p; p = p->next)
                if (!strcmp(v->string, p->string))
                    return false;
        if ((cJSON_IsObject(v) || cJSON_IsArray(v)) && !unique(v, depth + 1))
            return false;
    }
    return true;
}
/* Validate raw UTF-8 as cJSON does not reject malformed UTF-8 byte sequences.
 */
static bool utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], remaining = 0, minimum = 0;
        if (!c)
            return false;
        if (c < 128)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            remaining = 1;
            minimum = 128;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            remaining = 2;
            minimum = 2048;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            remaining = 3;
            minimum = 65536;
            c &= 7;
        } else
            return false;
        if (i + remaining > n)
            return false;
        while (remaining--) {
            if ((s[i] & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (s[i++] & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
static cJSON *parse(const char *json, size_t size, size_t limit) {
    if (!json || !size || size > limit || !utf8((const unsigned char *)json, size))
        return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        char c = json[i];
        if (quoted) {
            if (escaped) {
                if (c == 'u' && i + 5 <= size && !memcmp(json + i, "u0000", 5))
                    return NULL;
                escaped = false;
            } else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{') {
            if (++depth > 12)
                return NULL;
        } else if (c == ']' || c == '}') {
            if (!depth)
                return NULL;
            --depth;
        }
    }
    if (quoted || depth)
        return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!root)
        return NULL;
    while (end < json + size && strchr(" \r\n\t", *end))
        ++end;
    if (end != json + size || !unique(root, 0)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}
static bool add_file(ideas_package_t *p, const char *path, uint32_t size, const char *sha) {
    if (!ideas_path_valid(path) || !strcasecmp(path, "manifest.json") || !size || !hash(sha) ||
        p->file_count >= IDEAS_MAX_FILES - 1 || p->total_size + size > IDEAS_MAX_PACKAGE_BYTES)
        return false;
    for (unsigned i = 0; i < p->file_count; ++i) {
        size_t a = strlen(path), b = strlen(p->files[i].name), n = a < b ? a : b;
        if (!strncasecmp(path, p->files[i].name, n) &&
            (a == b || path[n] == '/' || p->files[i].name[n] == '/'))
            return false;
    }
    ideas_file_t *f = &p->files[p->file_count++];
    strcpy(f->name, path);
    strcpy(f->sha256, sha);
    f->size = size;
    snprintf(f->path, sizeof(f->path), "/api/v1/ideas/files/%s", sha);
    p->total_size += size;
    return true;
}
static bool reference(ideas_package_t *p, const cJSON *obj, uint32_t maximum) {
    uint32_t size;
    return keys(obj, "|path||size||sha256|", 3, 3) && number(obj, "size", &size) &&
           size <= maximum && add_file(p, str(obj, "path"), size, str(obj, "sha256"));
}
esp_err_t ideas_package_parse(const char *json, size_t size, const char *code,
                              ideas_package_t *out) {
    if (!out || !ideas_local_code_valid(code))
        return ESP_ERR_INVALID_ARG;
    cJSON *root = parse(json, size, IDEAS_DESCRIPTOR_BYTES);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->root_size = size;
    out->total_size = size;
    bool local = !ideas_code_valid(code);
    const cJSON *app = get(root, "application"), *fw = get(root, "firmware"),
                *dev = get(root, "device");
    if (!keys(root, "|schema||release_id||application||firmware||device|", 5, 5) ||
        strcmp(str(root, "schema"), local ? "mosaico-ideas/local/2" : "mosaico-ideas/device-release/1") ||
        !copy(root, "release_id", out->release_id, sizeof(out->release_id)) ||
        !identity(out->release_id) || !(local ? keys(app, "|id||code||name||description|", 4, 4) : keys(app, "|id||code|", 2, 2)) ||
        strcmp(str(app, "code"), code) ||
        !copy(app, "id", out->application_id, sizeof(out->application_id)) ||
        !identity(out->application_id) || !keys(fw, "|manifest|", 1, 1) ||
        strcmp(str(get(fw, "manifest"), "path"), "firmware/manifest.json") ||
        !reference(out, get(fw, "manifest"), IDEAS_MANIFEST_BYTES) ||
        !keys(dev, "|profile||default_locale||presentations|", 3, 3) ||
        strcmp(str(dev, "profile"), "esp-mosaico/v2") ||
        !copy(dev, "default_locale", out->default_locale, sizeof(out->default_locale)) ||
        (strcmp(out->default_locale, "zh-CN") && strcmp(out->default_locale, "en")))
        goto done;
    strcpy(out->code, code);
    const cJSON *views = get(dev, "presentations");
    if (local) {
        if (!cJSON_IsArray(views) || cJSON_GetArraySize(views) ||
            !copy(app, "name", out->name, sizeof(out->name)) || !*out->name ||
            !copy(app, "description", out->description, sizeof(out->description))) goto done;
        strcpy(out->locale, out->default_locale);
        if (!ideas_hash_data(json, size, out->root_sha256)) { err = ESP_FAIL; goto done; }
        err = ESP_OK;
        goto done;
    }
    if (!cJSON_IsArray(views) || cJSON_GetArraySize(views) != 2)
        goto done;
    bool seen[2] = {false};
    for (unsigned i = 0; i < 2; ++i) {
        const cJSON *v = cJSON_GetArrayItem(views, i), *display = get(v, "display"),
                    *assets = get(v, "assets");
        const char *path = str(display, "path");
        unsigned language;
        if (!strcmp(path, "device/zh-CN/display.json"))
            language = 0;
        else if (!strcmp(path, "device/en/display.json"))
            language = 1;
        else
            goto done;
        if (seen[language] || !keys(v, "|display||assets|", 2, 2))
            goto done;
        seen[language] = true;
        out->display_index[language] = out->file_count;
        uint64_t before = out->total_size;
        if (!reference(out, display, 32768) || !cJSON_IsArray(assets))
            goto done;
        int count = cJSON_GetArraySize(assets);
        if (count < 1 || count > 3)
            goto done;
        out->asset_count[language] = count;
        const char *prefix = language ? "device/en/" : "device/zh-CN/";
        for (int j = 0; j < count; ++j) {
            const cJSON *a = cJSON_GetArrayItem(assets, j);
            const char *name = str(a, "path");
            if (strncmp(name, prefix, strlen(prefix)) ||
                !ideas_filename_valid(name + strlen(prefix)))
                goto done;
            out->asset_index[language][j] = out->file_count;
            if (!reference(out, a, IDEAS_MAX_ASSET_BYTES))
                goto done;
        }
        if (out->total_size - before > IDEAS_MAX_ASSET_BYTES)
            goto done;
    }
    if (!ideas_hash_data(json, size, out->root_sha256)) {
        err = ESP_FAIL;
        goto done;
    }
    err = ESP_OK;
done:
    cJSON_Delete(root);
    return err;
}
esp_err_t ideas_package_firmware(ideas_package_t *out, const char *json, size_t size) {
    if (!out || !out->file_count || out->firmware_ready)
        return ESP_ERR_INVALID_ARG;
    char actual[65];
    if (size != out->files[0].size)
        return ESP_ERR_INVALID_CRC;
    if (!ideas_hash_data(json, size, actual)) return ESP_FAIL;
    if (strcmp(actual, out->files[0].sha256))
        return ESP_ERR_INVALID_CRC;
    cJSON *root = parse(json, size, IDEAS_MANIFEST_BYTES);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    uint32_t chip;
    const cJSON *target = get(root, "target"), *components = get(root, "components");
    if (!keys(root,
              "|schema||release||minimum_recovery_version||target||target_layout_"
              "sha256||components|",
              5, 6) ||
        strcmp(str(root, "schema"), "esp-iris-system-update/0.2") ||
        !copy(root, "release", out->version, sizeof(out->version)) || !*out->version ||
        !copy(root, "target_layout_sha256", out->layout_sha256, sizeof(out->layout_sha256)) ||
        !hash(out->layout_sha256) || !keys(target, "|chip_id||flash_size|", 2, 2) ||
        !number(target, "chip_id", &chip) || chip != 32 ||
        !number(target, "flash_size", &out->flash_size) || !out->flash_size ||
        out->flash_size > IDEAS_MAX_PACKAGE_BYTES || out->flash_size % 4096 ||
        !cJSON_IsArray(components))
        goto done;
    if (get(root, "minimum_recovery_version") &&
        (!copy(root, "minimum_recovery_version", out->minimum_recovery_version,
               sizeof(out->minimum_recovery_version)) ||
         !*out->minimum_recovery_version))
        goto done;
    int count = cJSON_GetArraySize(components);
    if (count < 2 || count > 8)
        goto done;
    uint32_t ids[8], offsets[8], sizes[8];
    unsigned applications = 0;
    for (int i = 0; i < count; ++i) {
        const cJSON *c = cJSON_GetArrayItem(components, i);
        const char *kind = str(c, "kind"), *name = str(c, "file"), *sha = str(c, "sha256");
        uint32_t flags = 0;
        if (!keys(c, "|id||kind||target_offset||size||sha256||file||flags|", 6, 7) ||
            !ideas_filename_valid(name) || !number(c, "id", &ids[i]) || !ids[i] || ids[i] > 255 ||
            !number(c, "target_offset", &offsets[i]) || !number(c, "size", &sizes[i]) ||
            !sizes[i] || sizes[i] > 16U * 1024U * 1024U || !hash(sha) ||
            (get(c, "flags") && (!number(c, "flags", &flags) || flags)) ||
            (uint64_t)offsets[i] + sizes[i] > out->flash_size || offsets[i] % 4096)
            goto done;
        if (!i) {
            if (strcmp(kind, "partition_table") || offsets[i] != 0x8000 || sizes[i] != 4096 ||
                strcmp(sha, out->layout_sha256))
                goto done;
        } else if (!strcmp(kind, "application")) {
            if (++applications != 1 || offsets[i] != 0x210000)
                goto done;
            out->application_offset = offsets[i];
            out->application_size = sizes[i];
            strcpy(out->application_sha256, sha);
        } else if (strcmp(kind, "data") || offsets[i] < 0x210000)
            goto done;
        for (int j = 0; j < i; ++j)
            if (ids[i] == ids[j] || ((uint64_t)offsets[i] < (uint64_t)offsets[j] + sizes[j] &&
                                     (uint64_t)offsets[j] < (uint64_t)offsets[i] + sizes[i]))
                goto done;
        char path[80];
        snprintf(path, sizeof(path), "firmware/%s", name);
        if (!add_file(out, path, sizes[i], sha))
            goto done;
    }
    if (applications != 1)
        goto done;
    memcpy(out->manifest, json, size);
    out->manifest[size] = 0;
    out->component_count = count;
    out->firmware_ready = true;
    err = ESP_OK;
done:
    cJSON_Delete(root);
    return err;
}
esp_err_t ideas_package_display(ideas_package_t *p, unsigned language, const char *json,
                                size_t size) {
    if (!p || language > 1 || !p->file_count)
        return ESP_ERR_INVALID_ARG;
    const ideas_file_t *display = &p->files[p->display_index[language]];
    char actual[65];
    if (size != display->size)
        return ESP_ERR_INVALID_CRC;
    if (!ideas_hash_data(json, size, actual)) return ESP_FAIL;
    if (strcmp(actual, display->sha256))
        return ESP_ERR_INVALID_CRC;
    cJSON *root = parse(json, size, 32768);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *locale = language ? "en" : "zh-CN";
    const cJSON *name = get(root, "name"), *description = get(root, "description");
    if (!keys(root, "|schema||locale||icon||name||description|", 5, 5) ||
        strcmp(str(root, "schema"), "mosaico-ideas/display/1") ||
        strcmp(str(root, "locale"), locale) || !copy(name, "text", p->name, sizeof(p->name)) ||
        !*p->name || !copy(description, "text", p->description, sizeof(p->description)) ||
        !cJSON_IsString(get(root, "icon")))
        goto done;
    strcpy(p->locale, locale);
    strcpy(p->presentation, display->name);
    p->font_count = 0;
    memset(p->fonts, 0, sizeof(p->fonts));
    p->icon[0] = 0;
    const char *refs[3] = {str(name, "font"), str(description, "font"), str(root, "icon")};
    bool used[3] = {false};
    for (unsigned r = 0; r < 3; ++r) {
        if (r == 2 && !*refs[r])
            continue;
        if (!ideas_filename_valid(refs[r]))
            goto done;
        char path[96];
        snprintf(path, sizeof(path), "device/%s/%s", locale, refs[r]);
        unsigned index = 0;
        for (; index < p->asset_count[language]; ++index)
            if (!strcmp(path, p->files[p->asset_index[language][index]].name))
                break;
        if (index == p->asset_count[language])
            goto done;
        if (r == 2) {
            if (used[index] || p->files[p->asset_index[language][index]].size > 65536)
                goto done;
            strcpy(p->icon, path);
        } else if (!used[index])
            strcpy(p->fonts[p->font_count++], path);
        used[index] = true;
    }
    for (unsigned i = 0; i < p->asset_count[language]; ++i)
        if (!used[i])
            goto done;
    err = ESP_OK;
done:
    cJSON_Delete(root);
    return err;
}
