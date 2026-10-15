// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IDEAS_CODE_BYTES 7
#define IDEAS_NAME_BYTES 129
#define IDEAS_PATH_BYTES 256
#define IDEAS_MAX_FILES 32
#define IDEAS_MAX_FONTS 2
#define IDEAS_MAX_ITEMS 16
#define IDEAS_DESCRIPTOR_BYTES 65536
#define IDEAS_MANIFEST_BYTES 3072
#define IDEAS_MAX_ASSET_BYTES (1024U * 1024U)
#define IDEAS_MAX_PACKAGE_BYTES (32U * 1024U * 1024U)

typedef struct {
    char name[193], path[193], sha256[65];
    uint32_t size;
} ideas_file_t;

typedef struct {
    char code[IDEAS_CODE_BYTES], application_id[65], release_id[65], version[65];
    char name[IDEAS_NAME_BYTES], description[513], locale[24];
    char presentation[193], icon[193];
    char application_sha256[65], layout_sha256[65];
    uint32_t application_size, application_offset, flash_size;
    size_t file_count, component_count;
    uint64_t total_size;
    ideas_file_t files[IDEAS_MAX_FILES];
    char manifest[IDEAS_MANIFEST_BYTES + 1];
    size_t font_count;
    char fonts[IDEAS_MAX_FONTS][193];
    char root_sha256[65], minimum_recovery_version[65], default_locale[8];
    size_t root_size;
    unsigned display_index[2], asset_count[2], asset_index[2][3];
    bool firmware_ready;
} ideas_package_t;

bool ideas_code_valid(const char *code);
/* Local developer copies use d + five hex digits, never sent to the cloud. */
bool ideas_local_code_valid(const char *code);
bool ideas_filename_valid(const char *name);
bool ideas_path_valid(const char *path);
/* Root metadata and dependencies are parsed before fetching the inner manifest.
 * name is the local relative path; path is a derived content-addressed HTTP
 * path. */
esp_err_t ideas_package_parse(const char *json, size_t size, const char *code,
                              ideas_package_t *out);
esp_err_t ideas_package_firmware(ideas_package_t *out, const char *json, size_t size);
esp_err_t ideas_package_display(ideas_package_t *out, unsigned language, const char *json,
                                size_t size);
bool ideas_hash_data(const void *data, size_t size, char out[65]);
bool ideas_hash_file(const char *path, uint32_t size, const char *expected);
/* Return an error to stop before the next read. Cancellation is propagated
 * separately from integrity failure so recovery never replaces a cancelled generation. */
typedef esp_err_t (*ideas_hash_progress_fn)(uint32_t checked, void *ctx);
esp_err_t ideas_hash_file_progress(const char *path, uint32_t size, const char *expected,
                              ideas_hash_progress_fn progress, void *ctx);
bool ideas_png_valid(const void *data, size_t size);
void *ideas_title_qoi(const uint16_t *pixels, size_t *size);

typedef esp_err_t (*ideas_fetch_fn)(const ideas_file_t *file, const char *destination, void *ctx);
typedef esp_err_t (*ideas_verify_fn)(const ideas_file_t *file, const char *path, void *ctx);
typedef enum { IDEAS_STORE_VERIFY, IDEAS_STORE_IMAGE, IDEAS_STORE_SAVE } ideas_store_stage_t;
typedef void (*ideas_store_stage_fn)(ideas_store_stage_t stage, void *ctx);
/* All functions are called by one serialized library worker. root is the
 * dedicated Ideas directory, never the complete NAND volume. */
/* prepare returns a fully checked generation; refresh authenticates metadata
 * and recovers interrupted transactions. Installers must verify NOR contents.
 * finish may only consume the package just verified by download/prepare while
 * holding the storage lease, without yielding ownership to another writer. */
esp_err_t ideas_store_prepare(const char *root, const char *code, ideas_package_t *out);
/* Return ESP_ERR_INVALID_STATE to cancel without attempting .previous recovery. */
typedef esp_err_t (*ideas_store_progress_fn)(uint64_t checked, uint64_t total,
                                            unsigned verified, unsigned count, void *ctx);
esp_err_t ideas_store_prepare_progress(const char *root, const char *code, ideas_package_t *out,
                                       ideas_store_progress_fn progress, void *ctx);
esp_err_t ideas_store_refresh(const char *root, const char *code, ideas_package_t *out);
esp_err_t ideas_store_finish(const char *root, const ideas_package_t *verified);
esp_err_t ideas_store_recover(const char *root, const char *code);
esp_err_t ideas_store_metadata(const char *root, const char *code, ideas_package_t *out);
esp_err_t ideas_store_read(const char *root, const char *code, ideas_package_t *out);
esp_err_t ideas_store_select(const char *root, ideas_package_t *out, unsigned language);
esp_err_t ideas_store_download(const char *root, const char *json, size_t size,
                               const ideas_package_t *package, ideas_fetch_fn fetch,
                               ideas_verify_fn verify, void *ctx, ideas_store_stage_fn stage);
esp_err_t ideas_store_verify(const char *root, const ideas_package_t *package,
                             ideas_verify_fn verify, void *ctx);
esp_err_t ideas_store_path(const char *root, const char *code, const char *name,
                           char out[IDEAS_PATH_BYTES]);
esp_err_t ideas_store_delete(const char *root, const char *code);
/* Same transaction with a cancellation/commit gate before the rename boundary. */
esp_err_t ideas_store_download_gated(const char *root, const char *json, size_t size,
                                    const ideas_package_t *package, ideas_fetch_fn fetch,
                                    ideas_verify_fn verify, void *ctx, ideas_store_stage_fn stage,
                                    esp_err_t (*commit_gate)(void *));
