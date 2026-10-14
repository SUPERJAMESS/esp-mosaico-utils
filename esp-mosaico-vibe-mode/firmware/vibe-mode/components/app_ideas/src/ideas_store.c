// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "factory_io_metrics.h"
#include "ideas_presentation.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "factory_system_update.h"
/* The NAND LittleFS backend has no links. Host tests explicitly reject links.
 */
#define node_stat stat
#else
#define node_stat lstat
#endif

typedef struct {
    uint32_t size;
    char sha[65];
} root_record_t;
/* Internal commit journal, outside the immutable file collection. Both roots
 * remain authorized during the two atomic directory renames. */
typedef struct {
    uint32_t magic;
    char application_id[65];
    root_record_t roots[2];
} journal_t;
#define JOURNAL_MAGIC 0x49444531U
static esp_err_t join(const char *a, const char *b, char out[IDEAS_PATH_BYTES]) {
    int n = snprintf(out, IDEAS_PATH_BYTES, "%s/%s", a, b);
    return n > 0 && n < IDEAS_PATH_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static esp_err_t base_path(const char *root, const char *code, char out[IDEAS_PATH_BYTES]) {
    return root && ideas_code_valid(code) ? join(root, code, out) : ESP_ERR_INVALID_ARG;
}
esp_err_t ideas_store_path(const char *root, const char *code, const char *name,
                           char out[IDEAS_PATH_BYTES]) {
    if (!ideas_path_valid(name))
        return ESP_ERR_INVALID_ARG;
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES];
    esp_err_t err = base_path(root, code, base);
    if (err == ESP_OK)
        err = join(base, "current", current);
    return err == ESP_OK ? join(current, name, out) : err;
}
static bool directory(const char *path) {
    struct stat st;
    return node_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static esp_err_t mkdir_checked(const char *path) {
    return mkdir(path, 0700) == 0 || (errno == EEXIST && directory(path)) ? ESP_OK : ESP_FAIL;
}
static esp_err_t sync_directory(const char *path) {
#ifdef ESP_PLATFORM
    /* LittleFS makes rename/mkdir/remove durable as atomic metadata commits. */
    (void)path;
    return ESP_OK;
#else
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return ESP_FAIL;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? ESP_OK : ESP_FAIL;
#endif
}
static esp_err_t write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return ESP_FAIL;
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f))
        ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}
static void *read_file(const char *path, size_t maximum, size_t *size) {
    struct stat st;
    if (node_stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > maximum)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    void *data = malloc(st.st_size + 1);
    bool ok = data && fread(data, 1, st.st_size, f) == st.st_size && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!ok) {
        free(data);
        return NULL;
    }
    ((char *)data)[st.st_size] = 0;
    *size = st.st_size;
    return data;
}
static esp_err_t parents(const char *base, const char *name) {
    char path[IDEAS_PATH_BYTES];
    if (join(base, name, path) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    for (char *p = path + strlen(base) + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            esp_err_t err = mkdir_checked(path);
            *p = '/';
            if (err != ESP_OK)
                return err;
        }
    return ESP_OK;
}
static bool expected_path(const ideas_package_t *p, const char *relative, bool dir) {
    if (!dir && !strcmp(relative, "manifest.json"))
        return true;
    size_t n = strlen(relative);
    for (unsigned i = 0; i < p->file_count; ++i)
        if (dir ? (!strncmp(relative, p->files[i].name, n) && p->files[i].name[n] == '/')
                : !strcmp(relative, p->files[i].name))
            return true;
    return false;
}
/* Bounded traversal never follows links, including during cleanup. */
static esp_err_t walk(const char *base, const char *relative, unsigned depth,
                      const ideas_package_t *p, unsigned *entries, bool remove) {
    char path[IDEAS_PATH_BYTES];
    if (depth > 4 || join(base, relative, path) != ESP_OK || !directory(path))
        return ESP_FAIL;
    DIR *d = opendir(path);
    if (!d)
        return ESP_FAIL;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while (err == ESP_OK && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[IDEAS_PATH_BYTES], full[IDEAS_PATH_BYTES];
        int n =
            snprintf(child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        struct stat st;
        if (++*entries > 160 || n < 0 || n >= sizeof(child) || !ideas_path_valid(child) ||
            join(base, child, full) != ESP_OK || node_stat(full, &st)) {
            err = ESP_FAIL;
            break;
        }
        bool dir = S_ISDIR(st.st_mode);
        if ((!dir && !S_ISREG(st.st_mode)) || (p && !expected_path(p, child, dir))) {
            err = ESP_FAIL;
            break;
        }
        if (dir)
            err = walk(base, child, depth + 1, p, entries, remove);
        else if (remove && unlink(full))
            err = ESP_FAIL;
    }
    closedir(d);
    if (err == ESP_OK)
        err = sync_directory(path);
    if (remove && err == ESP_OK && rmdir(path))
        err = ESP_FAIL;
    return err;
}
static esp_err_t remove_generation(const char *path) {
    struct stat st;
    if (node_stat(path, &st))
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode))
        return ESP_FAIL;
    unsigned entries = 0;
    return walk(path, "", 0, NULL, &entries, true);
}
static const char *filename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}
static esp_err_t display_valid(const char *base, ideas_package_t *p, unsigned language) {
    char path[IDEAS_PATH_BYTES];
    if (join(base, p->files[p->display_index[language]].name, path) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    size_t size = 0;
    void *json = read_file(path, 32768, &size);
    esp_err_t err = json ? ideas_package_display(p, language, json, size) : ESP_ERR_INVALID_SIZE;
    ideas_display_sources_t sources = {.name = p->name,
                                       .description = p->description,
                                       .locale = p->locale,
                                       .icon = filename(p->icon),
                                       .font_count = p->font_count};
    void *fonts[2] = {0}, *icon = NULL;
    for (unsigned i = 0; err == ESP_OK && i < p->font_count; ++i) {
        err = join(base, p->fonts[i], path);
        if (err == ESP_OK)
            fonts[i] = read_file(path, IDEAS_MAX_ASSET_BYTES, &size);
        if (!fonts[i])
            err = ESP_ERR_INVALID_SIZE;
        sources.fonts[i] = (ideas_font_source_t){filename(p->fonts[i]), fonts[i], size};
    }
    if (err == ESP_OK && p->icon[0]) {
        err = join(base, p->icon, path);
        if (err == ESP_OK)
            icon = read_file(path, 65536, &size);
        if (!icon || !ideas_png_valid(icon, size))
            err = ESP_ERR_INVALID_ARG;
    }
    ideas_presentation_t *presentation = err == ESP_OK ? malloc(sizeof(*presentation)) : NULL;
    if (err == ESP_OK && !presentation)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && !ideas_presentation_parse(json, p->files[p->display_index[language]].size,
                                                   &sources, presentation))
        err = ESP_ERR_INVALID_ARG;
    free(presentation);
    free(icon);
    free(fonts[0]);
    free(fonts[1]);
    free(json);
    return err;
}
static esp_err_t load_generation(const char *base, const char *code, const journal_t *journal,
                                 ideas_package_t *p, bool complete, bool hash_files) {
    char path[IDEAS_PATH_BYTES];
    if (!directory(base) || join(base, "manifest.json", path) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    size_t size;
    char *json = read_file(path, IDEAS_DESCRIPTOR_BYTES, &size);
    if (!json)
        return ESP_ERR_NOT_FOUND;
    esp_err_t err = ideas_package_parse(json, size, code, p);
    free(json);
    bool trusted = false;
    if (err == ESP_OK)
        for (unsigned i = 0; i < 2; ++i)
            trusted |=
                journal->roots[i].size == size && !strcmp(journal->roots[i].sha, p->root_sha256);
    if (!trusted || strcmp(journal->application_id, p->application_id))
        return ESP_ERR_INVALID_CRC;
    if (join(base, p->files[0].name, path) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    json = read_file(path, IDEAS_MANIFEST_BYTES, &size);
    if (!json)
        return ESP_ERR_INVALID_SIZE;
    err = ideas_package_firmware(p, json, size);
    free(json);
    if (!complete) return err;
    unsigned entries = 0;
    if (err == ESP_OK)
        err = walk(base, "", 0, p, &entries, false);
    for (unsigned i = 0; hash_files && err == ESP_OK && i < p->file_count; ++i) {
        err = join(base, p->files[i].name, path);
        if (err == ESP_OK && !ideas_hash_file(path, p->files[i].size, p->files[i].sha256))
            err = ESP_ERR_INVALID_CRC;
    }
    /* Validate both locales; return metadata for the package's default locale. */
    unsigned active = !strcmp(p->default_locale, "en");
    if (err == ESP_OK)
        err = display_valid(base, p, 1 - active);
    if (err == ESP_OK)
        err = display_valid(base, p, active);
    return err;
}
static bool read_journal(const char *base, journal_t *out) {
    char path[IDEAS_PATH_BYTES];
    size_t size;
    if (join(base, ".roots", path) != ESP_OK)
        return false;
    void *raw = read_file(path, sizeof(*out), &size);
    bool valid = raw && size == sizeof(*out);
    if (valid)
        memcpy(out, raw, size);
    free(raw);
    if (!valid || out->magic != JOURNAL_MAGIC || out->application_id[64] ||
        !ideas_path_valid(out->application_id) || strchr(out->application_id, '/'))
        return false;
    for (unsigned i = 0; i < 2; ++i)
        if (out->roots[i].size &&
            (out->roots[i].size > IDEAS_DESCRIPTOR_BYTES || out->roots[i].sha[64] ||
             strspn(out->roots[i].sha, "0123456789abcdef") != 64))
            return false;
    return true;
}
static esp_err_t save_journal(const char *base, const journal_t *journal) {
    char temp[IDEAS_PATH_BYTES], path[IDEAS_PATH_BYTES];
    if (join(base, ".roots.tmp", temp) != ESP_OK || join(base, ".roots", path) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = write_file(temp, journal, sizeof(*journal));
    if (err == ESP_OK && rename(temp, path))
        err = ESP_FAIL;
    return err == ESP_OK ? sync_directory(base) : err;
}
esp_err_t ideas_store_read(const char *root, const char *code, ideas_package_t *p) {
    if (!p)
        return ESP_ERR_INVALID_ARG;
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES];
    journal_t journal;
    esp_err_t err = base_path(root, code, base);
    if (err == ESP_OK)
        err = join(base, "current", current);
    if (err != ESP_OK)
        return err;
    if (!read_journal(base, &journal))
        return ESP_ERR_NOT_FOUND;
    return load_generation(current, code, &journal, p, true, true);
}
/* Display-only access authenticates root/inner manifests; selected assets are
 * still checked by the presentation loader. Never use this for commit/install. */
esp_err_t ideas_store_metadata(const char *root, const char *code, ideas_package_t *p) {
    if (!p)
        return ESP_ERR_INVALID_ARG;
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES];
    journal_t journal;
    esp_err_t err = base_path(root, code, base);
    if (err == ESP_OK)
        err = join(base, "current", current);
    if (err != ESP_OK)
        return err;
    if (!read_journal(base, &journal))
        return ESP_ERR_NOT_FOUND;
    return load_generation(current, code, &journal, p, false, false);
}
esp_err_t ideas_store_select(const char *root, ideas_package_t *p, unsigned language) {
    if (!p || language > 1)
        return ESP_ERR_INVALID_ARG;
    char path[IDEAS_PATH_BYTES];
    size_t size;
    esp_err_t err =
        ideas_store_path(root, p->code, p->files[p->display_index[language]].name, path);
    if (err != ESP_OK)
        return err;
    char *json = read_file(path, 32768, &size);
    if (!json)
        return ESP_ERR_INVALID_SIZE;
    err = ideas_package_display(p, language, json, size);
    free(json);
    return err;
}
/* Finish only the generation verified by this operation. This is not a
 * persistent validation cache: future installs still verify every source byte. */
esp_err_t ideas_store_finish(const char *root, const ideas_package_t *p) {
    char base[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES], incoming[IDEAS_PATH_BYTES];
    if (!p || base_path(root, p->code, base) != ESP_OK ||
        join(base, ".previous", previous) != ESP_OK || join(base, ".incoming", incoming) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    journal_t keep = {.magic = JOURNAL_MAGIC, .roots = {{.size = p->root_size}}};
    strcpy(keep.application_id, p->application_id);
    strcpy(keep.roots[0].sha, p->root_sha256);
    esp_err_t err = save_journal(base, &keep);
    if (err == ESP_OK)
        err = remove_generation(previous);
    if (err == ESP_OK)
        err = remove_generation(incoming);
    return err;
}

esp_err_t ideas_store_prepare(const char *root, const char *code, ideas_package_t *p) {
    if (!p)
        return ESP_ERR_INVALID_ARG;
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES],
        incoming[IDEAS_PATH_BYTES];
    if (base_path(root, code, base) != ESP_OK || join(base, "current", current) != ESP_OK ||
        join(base, ".previous", previous) != ESP_OK || join(base, ".incoming", incoming) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    journal_t journal;
    if (!read_journal(base, &journal))
        return ESP_ERR_INVALID_CRC;
    esp_err_t err = load_generation(current, code, &journal, p, true, true);
    if (err != ESP_OK) {
        err = load_generation(previous, code, &journal, p, true, true);
        if (err == ESP_OK)
            err = remove_generation(current);
        if (err == ESP_OK && rename(previous, current))
            err = ESP_FAIL;
    }
    if (err == ESP_OK)
        err = ideas_store_finish(root, p);
    factory_io_report("prepare", code, &metrics, NULL, 0, err);
    return err;
}

esp_err_t ideas_store_recover(const char *root, const char *code) {
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES],
        incoming[IDEAS_PATH_BYTES];
    if (base_path(root, code, base) != ESP_OK || join(base, "current", current) != ESP_OK ||
        join(base, ".previous", previous) != ESP_OK || join(base, ".incoming", incoming) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (!directory(current) && !directory(previous))
        return remove_generation(incoming);
    ideas_package_t *p = malloc(sizeof(*p));
    if (!p)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ideas_store_prepare(root, code, p);
    free(p);
    return err;
}

/* Stable library scans read authenticated descriptors and selected display
 * assets only. An interrupted journal always takes the full recovery path. */
esp_err_t ideas_store_refresh(const char *root, const char *code, ideas_package_t *p) {
    char base[IDEAS_PATH_BYTES], path[IDEAS_PATH_BYTES];
    journal_t journal;
    if (base_path(root, code, base) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    bool clean = read_journal(base, &journal) && !journal.roots[1].size;
    const char *pending[] = {".incoming", ".previous", ".roots.tmp"};
    for (unsigned i = 0; clean && i < 3; ++i) {
        struct stat st;
        clean = join(base, pending[i], path) == ESP_OK && node_stat(path, &st) != 0 &&
                errno == ENOENT;
    }
    return clean ? ideas_store_metadata(root, code, p) : ideas_store_prepare(root, code, p);
}
esp_err_t ideas_store_verify(const char *root, const ideas_package_t *p, ideas_verify_fn verify,
                             void *ctx) {
    if (!p || !verify || !p->firmware_ready)
        return ESP_ERR_INVALID_ARG;
    char path[IDEAS_PATH_BYTES];
    if (ideas_store_path(root, p->code, "manifest.json", path) != ESP_OK ||
        !ideas_hash_file(path, p->root_size, p->root_sha256))
        return ESP_ERR_INVALID_CRC;
    for (unsigned i = 0; i < p->file_count; ++i) {
        esp_err_t err = ideas_store_path(root, p->code, p->files[i].name, path);
        if (err == ESP_OK)
            err = verify(&p->files[i], path, ctx);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}
esp_err_t ideas_store_download(const char *root, const char *json, size_t size,
                               const ideas_package_t *p, ideas_fetch_fn fetch,
                               ideas_verify_fn verify, void *ctx, ideas_store_stage_fn stage) {
    char sha[65];
    if (!p || !fetch || !verify || !p->firmware_ready || size != p->root_size ||
        !ideas_hash_data(json, size, sha) || strcmp(sha, p->root_sha256))
        return ESP_ERR_INVALID_ARG;
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES],
        incoming[IDEAS_PATH_BYTES], path[IDEAS_PATH_BYTES];
    if (base_path(root, p->code, base) != ESP_OK || join(base, "current", current) != ESP_OK ||
        join(base, ".previous", previous) != ESP_OK || join(base, ".incoming", incoming) != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = mkdir_checked(root);
    if (err == ESP_OK)
        err = mkdir_checked(base);
    journal_t existing;
    bool bound = read_journal(base, &existing);
    if (err == ESP_OK && bound && strcmp(existing.application_id, p->application_id))
        return ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        ideas_package_t *old_package = malloc(sizeof(*old_package));
        err = old_package ? (directory(current) || directory(previous)
                                  ? ideas_store_refresh(root, p->code, old_package)
                                  : remove_generation(incoming)) : ESP_ERR_NO_MEM;
        free(old_package);
        /* A fresh complete collection may repair the sole corrupt current
         * generation. Keep it until the replacement is fully verified. */
        if (err != ESP_OK && bound && directory(current) && !directory(previous))
            err = remove_generation(incoming);
    }
    if (err == ESP_OK)
        err = mkdir_checked(incoming);
    if (err != ESP_OK)
        return err;
    for (unsigned i = 0; i < p->file_count && err == ESP_OK; ++i) {
        err = parents(incoming, p->files[i].name);
        if (err == ESP_OK)
            err = join(incoming, p->files[i].name, path);
        if (err == ESP_OK)
            err = fetch(&p->files[i], path, ctx);
    }
    if (err == ESP_OK && stage)
        stage(IDEAS_STORE_VERIFY, ctx);
    for (unsigned i = 0; i < p->file_count && err == ESP_OK; ++i) {
        err = join(incoming, p->files[i].name, path);
        if (err == ESP_OK)
            err = verify(&p->files[i], path, ctx);
    }
    if (err == ESP_OK && stage)
        stage(IDEAS_STORE_IMAGE, ctx);
    if (err == ESP_OK)
        err = join(incoming, "manifest.json", path);
    if (err == ESP_OK)
        err = write_file(path, json, size);
    journal_t journal = {.magic = JOURNAL_MAGIC, .roots = {{.size = size}}};
    strcpy(journal.application_id, p->application_id);
    strcpy(journal.roots[0].sha, sha);
    ideas_package_t *check = err == ESP_OK ? malloc(sizeof(*check)) : NULL;
    if (err == ESP_OK && !check)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = load_generation(incoming, p->code, &journal, check, true, false);
#ifdef ESP_PLATFORM
    if (err == ESP_OK)
        err = join(incoming, "firmware", path);
    if (err == ESP_OK)
        err = factory_system_update_check_idea(check->manifest, path);
#endif
    bool replacing = directory(current);
    journal_t old;
    if (err == ESP_OK && replacing) {
        if (!read_journal(base, &old))
            err = ESP_ERR_INVALID_CRC;
        else
            journal.roots[1] = old.roots[0];
    }
    free(check);
    if (err == ESP_OK && stage)
        stage(IDEAS_STORE_SAVE, ctx);
    /* Flush new files and directories before authorizing the root for recovery.
     */
    if (err == ESP_OK)
        err = sync_directory(incoming);
    if (err == ESP_OK)
        err = save_journal(base, &journal);
    if (err == ESP_OK && replacing && rename(current, previous))
        err = ESP_FAIL;
    if (err == ESP_OK)
        err = sync_directory(base);
    if (err == ESP_OK && rename(incoming, current))
        err = ESP_FAIL;
    if (err == ESP_OK)
        err = sync_directory(base);
    if (err != ESP_OK) {
        /* Keep either valid generation; recovery chooses only a verified root. */
        (void)ideas_store_recover(root, p->code);
        return err;
    }
    /* Retain previous as a fallback until the next verified recovery/scan. */
    return ESP_OK;
}
