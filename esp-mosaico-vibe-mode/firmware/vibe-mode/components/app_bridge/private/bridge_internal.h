// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "iris_bridge.h"
#include "mosaico_boot.h"
#include "system_plan.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "factory_system_metadata.h"
#include "factory_system_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 98304 /* 32 KiB manifest plus bounded image descriptors. */
typedef struct {
    int status;
    unsigned retry_after_seconds;
} http_response_t;
typedef struct {
    iris_bridge_config_t cfg;
    char session[80], token[160], boot_id[33], mac[18];
    atomic_bool stop_requested, bridge_running, bridge_active;
    http_response_t main_response, download_response;
    /* Worker, cancellation observer and final reporter never concurrently own
     * the same client. Joining observers precedes handle reuse or teardown. */
    esp_http_client_handle_t main_client, download_client;
    int64_t write_deadline;
    atomic_uint telemetry_revision, telemetry_progress;
    atomic_bool writing_acknowledged, report_running;
    esp_err_t final_result;
    atomic_bool cancelled, cancel_poll_running, cancel_poll_stop;
    char server_url[IRIS_BRIDGE_ORIGIN_BYTES];
    char board_id[IRIS_BRIDGE_BOARD_BYTES];
    char device_id[33];
    portMUX_TYPE snapshot_lock;
    iris_bridge_snapshot_t snapshot;
    int64_t code_deadline;
} bridge_context_t;
typedef struct {
    uint32_t version;
    char target[17];
    char table_hash[65];
} boot_record_t;

/* Component-private operations; mutable state is always passed explicitly. */
void bridge_set_state(bridge_context_t *ctx, const char *state, unsigned progress, esp_err_t error);
const char *bridge_str(const cJSON *o, const char *key);
uint32_t bridge_num(const cJSON *o, const char *key);
void bridge_hex(const uint8_t *in, size_t n, char *out);
bool bridge_digest(const void *p, size_t n, uint8_t out[32]);
bool bridge_matches(const void *p, size_t n, const char *expected);
bool bridge_flash_hash(uint32_t offset, uint32_t size, char out[65]);
esp_err_t bridge_response_header(esp_http_client_event_t *event);
esp_http_client_handle_t bridge_open_request_with_response(bridge_context_t *ctx, const char *path,
                                                           const char *body,
                                                           http_response_t *response,
                                                           esp_http_client_handle_t h,
                                                           int timeout_ms);
void bridge_close_client(esp_http_client_handle_t *client);
esp_http_client_handle_t bridge_open_request(bridge_context_t *ctx, const char *path,
                                             const char *body);
void bridge_finish_download(bridge_context_t *ctx, bool valid);
cJSON *bridge_request_with_response(bridge_context_t *ctx, const char *path, const cJSON *body,
                                    http_response_t *response, esp_http_client_handle_t *reusable,
                                    int timeout_ms, bool observer);
cJSON *bridge_request(bridge_context_t *ctx, const char *path, const cJSON *body);
void bridge_path_session(bridge_context_t *ctx, char out[256], const char *suffix);
bool bridge_event_with_timeout(bridge_context_t *ctx, const char *state, unsigned progress,
                               esp_err_t error, int timeout_ms);
bool bridge_terminal_event(bridge_context_t *ctx, const char *state, unsigned progress,
                           esp_err_t error);
bool bridge_should_cancel(bridge_context_t *ctx);
void bridge_notify_progress(bridge_context_t *ctx, unsigned progress);
void bridge_cancel_poll_run(void *unused);
esp_err_t bridge_cancel_poll_start(bridge_context_t *ctx);
void bridge_cancel_poll_join(bridge_context_t *ctx);
cJSON *bridge_identity(bridge_context_t *ctx);
esp_err_t bridge_save_boot(const cJSON *plan);
bool bridge_inventory(bridge_context_t *ctx);
esp_err_t bridge_get_table(bridge_context_t *ctx, const cJSON *plan, uint8_t raw[4096]);
cJSON *bridge_component(unsigned id, const char *kind, uint32_t offset, uint32_t size,
                        const char *sha, const char *file);
esp_err_t bridge_transfer(bridge_context_t *ctx, const char *file,
                          const esp_iris_system_update_component_t *c, const uint8_t *table);
bool bridge_authorize_commit(bridge_context_t *ctx);
void bridge_report_result_run(void *unused);
void bridge_report_result_bounded(bridge_context_t *ctx, esp_err_t result);
esp_err_t bridge_execute(bridge_context_t *ctx, const char *op, const cJSON *plan);
void bridge_run(void *unused);
