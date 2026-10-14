// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "factory_ideas.h"
#include "ideas_package.h"
#include "app_storage.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include "factory_system_metadata.h"
#include "ideas_presentation.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/nand/ideas"
#define IO_BYTES 4096
typedef struct ideas_context ideas_context_t;
typedef struct {
    void *data;
    size_t size;
} icon_t;
typedef struct {
    ideas_package_t package;
    char *json;
    size_t json_size;
    icon_t assets[IDEAS_MAX_FILES];
    icon_t pixels[2];
    bool already_downloaded, view_only;
    ideas_context_t *owner;
} pending_t;
typedef struct {
    factory_ideas_command_t command;
    char code[7];
    uint32_t request_id;
    pending_t *pending;
    bool up_to_date;
    ideas_context_t *owner;
} job_t;
struct ideas_context {
    SemaphoreHandle_t lock;
    factory_ideas_snapshot_t *state;
    icon_t icons[IDEAS_MAX_ITEMS];
    pending_t *pending;
    uint32_t request_id;
};
typedef struct {
    uint32_t schema, offset, size;
    char code[7], application_id[65], release_id[65], image[65], layout[65];
} boot_record_t;
_Static_assert(FACTORY_IDEAS_MAX_ITEMS == IDEAS_MAX_ITEMS, "snapshot capacity");

/* Private operations share the worker context; preview resources remain owned
 * until confirmation, dismissal or cancellation completion. */
bool ideas_cancelled(ideas_context_t *ctx, uint32_t request_id);
void ideas_free_pending(pending_t *p);
void ideas_message(ideas_context_t *ctx, const char *title, const char *detail);
void ideas_hex(const uint8_t digest[32], char out[65]);
esp_err_t ideas_verify_file(const ideas_file_t *file, const char *path, void *ctx);
esp_err_t ideas_flash_hash(uint32_t offset, uint32_t size, const char *expected);
bool ideas_boot_record(boot_record_t *record);
esp_err_t ideas_save_boot(void *ctx);
esp_err_t ideas_scan(ideas_context_t *ctx);
esp_err_t ideas_http_open(const char *path, const char *body, esp_http_client_handle_t *out);
esp_err_t ideas_resolve(ideas_context_t *ctx, const job_t *job, const ideas_package_t *local,
                        char **json, size_t *size);
esp_err_t ideas_fetch_file(const ideas_file_t *file, const char *path, void *argument);
bool ideas_ascii_text(const char *text);
int ideas_asset_index(const ideas_package_t *p, const char *name);
esp_err_t ideas_fetch_asset(ideas_context_t *ctx, const job_t *job, pending_t *pending,
                            unsigned index, bool local);
esp_err_t ideas_prepare_assets(ideas_context_t *ctx, const job_t *job, pending_t *pending,
                               bool local);
void ideas_preview_item(factory_idea_t *item, const ideas_package_t *p);
esp_err_t ideas_prepare_download(ideas_context_t *ctx, job_t *job);
esp_err_t ideas_run_local(ideas_context_t *ctx, job_t *job);
void ideas_worker(void *arg);
esp_err_t ideas_launch_locked(ideas_context_t *ctx, job_t *job);
