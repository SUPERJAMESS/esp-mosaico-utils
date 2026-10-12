// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static esp_err_t path_join(const char *root, const char *code, const char *suffix,
                           char path[IDEAS_PATH_BYTES])
{
    if (!root || !ideas_code_valid(code)) return ESP_ERR_INVALID_ARG;
    int n = snprintf(path, IDEAS_PATH_BYTES, "%s/%s/%s", root, code, suffix);
    return n > 0 && n < IDEAS_PATH_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t ideas_store_path(const char *root, const char *code, const char *name,
                          char out[IDEAS_PATH_BYTES])
{
    if (!name || (!ideas_filename_valid(name) && strcmp(name, "manifest.json") && strcmp(name, "idea.json")))
        return ESP_ERR_INVALID_ARG;
    char suffix[96];
    snprintf(suffix, sizeof(suffix), "current/%s", name);
    return path_join(root, code, suffix, out);
}
static bool directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static esp_err_t make_directory(const char *path)
{
    return mkdir(path, 0700) == 0 || (errno == EEXIST && directory(path)) ? ESP_OK : ESP_FAIL;
}
/* Never recurse: a generation consists exclusively of bounded root-level
 * files. Unexpected subdirectories are left untouched and reported. */
static esp_err_t remove_generation(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char file[IDEAS_PATH_BYTES];
        int n = snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
        struct stat st;
        if (n <= 0 || n >= sizeof(file) || stat(file, &st) || !S_ISREG(st.st_mode) || unlink(file)) {
            err = ESP_FAIL; break;
        }
    }
    closedir(d);
    if (err == ESP_OK && rmdir(path)) err = ESP_FAIL;
    return err;
}
esp_err_t ideas_store_recover(const char *root, const char *code)
{
    char current[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES], incoming[IDEAS_PATH_BYTES];
    if (path_join(root, code, "current", current) != ESP_OK ||
        path_join(root, code, ".previous", previous) != ESP_OK ||
        path_join(root, code, ".incoming", incoming) != ESP_OK) return ESP_ERR_INVALID_ARG;
    /* An interrupted replacement never exposes a partial new generation. */
    if (!directory(current) && directory(previous) && rename(previous, current)) return ESP_FAIL;
    esp_err_t err = remove_generation(previous);
    if (err == ESP_OK) err = remove_generation(incoming);
    return err;
}
static esp_err_t write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
    if (ok && fsync(fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}
esp_err_t ideas_store_read(const char *root, const char *code, ideas_package_t *out)
{
    char path[IDEAS_PATH_BYTES];
    if (ideas_store_path(root, code, "idea.json", path) != ESP_OK) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    char *json = malloc(IDEAS_DESCRIPTOR_BYTES + 1);
    if (!json) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(json, 1, IDEAS_DESCRIPTOR_BYTES + 1, f);
    esp_err_t err = ferror(f) || n > IDEAS_DESCRIPTOR_BYTES ? ESP_ERR_INVALID_SIZE :
        ideas_package_parse(json, n, code, out);
    fclose(f); free(json);
    return err;
}
esp_err_t ideas_store_verify(const char *root, const ideas_package_t *p,
                            ideas_verify_fn verify, void *ctx)
{
    if (!p || !verify) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < p->file_count; i++) {
        char path[IDEAS_PATH_BYTES];
        esp_err_t err = ideas_store_path(root, p->code, p->files[i].name, path);
        if (err == ESP_OK) err = verify(&p->files[i], path, ctx);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
esp_err_t ideas_store_download(const char *root, const char *json, size_t size,
                              const ideas_package_t *p, ideas_fetch_fn fetch,
                              ideas_verify_fn verify, void *ctx)
{
    if (!p || !fetch || !verify || !size || size > IDEAS_DESCRIPTOR_BYTES) return ESP_ERR_INVALID_ARG;
    char base[IDEAS_PATH_BYTES], current[IDEAS_PATH_BYTES], previous[IDEAS_PATH_BYTES];
    char incoming[IDEAS_PATH_BYTES], path[IDEAS_PATH_BYTES];
    if (path_join(root, p->code, "", base) != ESP_OK ||
        path_join(root, p->code, "current", current) != ESP_OK ||
        path_join(root, p->code, ".incoming", incoming) != ESP_OK ||
        path_join(root, p->code, ".previous", previous) != ESP_OK) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = make_directory(root);
    if (err == ESP_OK) err = make_directory(base);
    if (err == ESP_OK) err = ideas_store_recover(root, p->code);
    if (err == ESP_OK) err = make_directory(incoming);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < p->file_count && err == ESP_OK; i++) {
        int n = snprintf(path, sizeof(path), "%s/%s", incoming, p->files[i].name);
        if (n < 0 || n >= sizeof(path)) { err = ESP_ERR_INVALID_SIZE; break; }
        err = fetch(&p->files[i], path, ctx);
        if (err == ESP_OK) err = verify(&p->files[i], path, ctx);
    }
    if (err == ESP_OK) {
        err = path_join(root, p->code, ".incoming/manifest.json", path);
        if (err == ESP_OK) err = write_file(path, p->manifest, strlen(p->manifest));
    }
    if (err == ESP_OK) {
        err = path_join(root, p->code, ".incoming/idea.json", path);
        if (err == ESP_OK) err = write_file(path, json, size);
    }
    if (err != ESP_OK) { (void)remove_generation(incoming); return err; }
    bool replacing = directory(current);
    if (replacing && rename(current, previous)) { (void)remove_generation(incoming); return ESP_FAIL; }
    if (rename(incoming, current)) {
        if (replacing) (void)rename(previous, current);
        (void)remove_generation(incoming);
        return ESP_FAIL;
    }
    /* The new complete generation is authoritative. Cleanup is retried by
     * recovery if interrupted; previous is never listed as another version. */
    (void)remove_generation(previous);
    return ESP_OK;
}
