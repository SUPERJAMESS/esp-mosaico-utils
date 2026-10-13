// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define IDEAS_CODE_BYTES 7
#define IDEAS_NAME_BYTES 129
#define IDEAS_PATH_BYTES 256
#define IDEAS_MAX_FILES 12
#define IDEAS_MAX_FONTS 2
#define IDEAS_MAX_ITEMS 16
#define IDEAS_DESCRIPTOR_BYTES 16384
#define IDEAS_MANIFEST_BYTES 3072
#define IDEAS_MAX_ASSET_BYTES (1024U * 1024U)
#define IDEAS_MAX_PACKAGE_BYTES (32U * 1024U * 1024U)

typedef struct {
    char name[65], path[193], sha256[65];
    uint32_t size;
} ideas_file_t;

typedef struct {
    char code[IDEAS_CODE_BYTES], application_id[65], release_id[65], version[65];
    char name[IDEAS_NAME_BYTES], description[513], locale[24];
    char presentation[65], icon[65];
    char application_sha256[65], layout_sha256[65];
    uint32_t application_size, application_offset, flash_size;
    size_t file_count, component_count;
    uint64_t total_size;
    ideas_file_t files[IDEAS_MAX_FILES];
    char manifest[IDEAS_MANIFEST_BYTES + 1];
    size_t font_count;
    char fonts[IDEAS_MAX_FONTS][65];
} ideas_package_t;

bool ideas_code_valid(const char *code);
bool ideas_filename_valid(const char *name);
/* Bounded cloud descriptor; component files remain local, root-level names.
 * Only ordinary application/data/table components can enter the Ideas library. */
esp_err_t ideas_package_parse(const char *json, size_t size, const char *code,
                             ideas_package_t *out);

typedef esp_err_t (*ideas_fetch_fn)(const ideas_file_t *file, const char *destination,
                                  void *ctx);
typedef esp_err_t (*ideas_verify_fn)(const ideas_file_t *file, const char *path,
                                   void *ctx);
/* All functions are called by one serialized library worker. root is the
 * dedicated Ideas directory, never the complete NAND volume. */
esp_err_t ideas_store_recover(const char *root, const char *code);
esp_err_t ideas_store_read(const char *root, const char *code, ideas_package_t *out);
esp_err_t ideas_store_download(const char *root, const char *json, size_t size,
                              const ideas_package_t *package, ideas_fetch_fn fetch,
                              ideas_verify_fn verify, void *ctx);
esp_err_t ideas_store_verify(const char *root, const ideas_package_t *package,
                            ideas_verify_fn verify, void *ctx);
esp_err_t ideas_store_path(const char *root, const char *code, const char *name,
                          char out[IDEAS_PATH_BYTES]);
