// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_storage.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "factory_ideas.h"
#include "factory_io_metrics.h"
#include "factory_locale.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include "factory_system_metadata.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ideas_package.h"
#include "ideas_presentation.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    bool up_to_date, metadata_only, startup;
    ideas_context_t *owner;
} job_t;
typedef struct {
    uint32_t schema, offset, size;
    char code[7], application_id[65], release_id[65], image[65], layout[65];
} boot_record_t;
struct ideas_context {
    boot_record_t verified_boot;
    char development_release_id[65];
    bool boot_verified, cancel_requested, library_loaded;
    atomic_bool initialized;
    SemaphoreHandle_t lock;
    QueueHandle_t jobs;
    TaskHandle_t worker;
    factory_ideas_snapshot_t *state;
    icon_t icons[IDEAS_MAX_ITEMS];
    struct {
        char code[7];
        unsigned language;
        uint32_t stamp;
        icon_t image;
    } titles[8];
    uint32_t title_clock;
    pending_t *pending;
    uint32_t request_id;
};
_Static_assert(FACTORY_IDEAS_MAX_ITEMS == IDEAS_MAX_ITEMS, "snapshot capacity");

/* Private operations share the worker context; preview resources remain owned
 * until confirmation, dismissal or cancellation completion. */
bool ideas_cancelled(ideas_context_t *ctx, uint32_t request_id);
void ideas_free_pending(pending_t *p);
void ideas_message(ideas_context_t *ctx, const char *title, const char *detail);
void ideas_hex(const uint8_t digest[32], char out[65]);
esp_err_t ideas_verify_file(const ideas_file_t *file, const char *path, void *ctx);
esp_err_t ideas_flash_hash(uint32_t offset, uint32_t size, const char *expected);
esp_err_t ideas_verify_installed(const ideas_package_t *package, ideas_context_t *ctx);
/* Optional identity projection may use the boolean helper. Mutation guards
 * must retain operational errors from the record reader and NOR verifier. */
bool ideas_boot_record(boot_record_t *record, bool verify);
esp_err_t ideas_read_boot_record(boot_record_t *record);
esp_err_t ideas_save_boot(void *ctx);
esp_err_t ideas_scan(ideas_context_t *ctx, bool refresh_development);
/* Caller holds the NAND lease. Lazy startup must not bypass save admission. */
esp_err_t ideas_load_library(ideas_context_t *ctx);
esp_err_t ideas_fetch_preview(ideas_context_t *ctx, const job_t *job, pending_t *pending);
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
esp_err_t ideas_prepare_row(ideas_context_t *ctx, job_t *job);
esp_err_t ideas_worker_init(ideas_context_t *ctx);
esp_err_t ideas_delete(ideas_context_t *ctx, const char *code);
esp_err_t ideas_check_space(uint64_t bytes);
esp_err_t ideas_update_all(ideas_context_t *ctx, job_t *job);
bool ideas_job_cancelled(ideas_context_t *ctx);
esp_err_t ideas_commit_download(void *argument);
esp_err_t ideas_run_development(void);
void ideas_development_status(ideas_context_t *ctx);
esp_err_t ideas_save_development(ideas_context_t *ctx);
esp_err_t ideas_launch_locked(ideas_context_t *ctx, job_t *job);
