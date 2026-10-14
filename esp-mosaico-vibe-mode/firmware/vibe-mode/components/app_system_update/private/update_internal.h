// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_attr.h"
#include "factory_system_update.h"
#include "mosaico_boot.h"
#include "factory_recovery_version.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_SPIRAM_XIP_FROM_PSRAM
#error "Recovery self-update requires CONFIG_SPIRAM_XIP_FROM_PSRAM"
#endif

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_iris_system_update.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/esp_flash_internal.h"
#include "esp_system.h"
#include "factory_system_metadata.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#define FACTORY_SYSTEM_MAX_COMPONENTS 96U
#define FACTORY_SOURCE_MANIFEST_BYTES 32768U
#define FACTORY_SYSTEM_SCHEMA "esp-iris-system-update/0.2"
#define FACTORY_SYSTEM_HASH_CHUNK_BYTES 1024U
#define FACTORY_SYSTEM_RESTART_DELAY_MS 1800U
#define FACTORY_SYSTEM_ESP32S31_CHIP_ID 0x20U
#define FACTORY_SYSTEM_FLASH_SECTOR_BYTES 0x1000U
#define FACTORY_SYSTEM_RECOVERY_OFFSET 0x20000U
#define FACTORY_SYSTEM_RECOVERY_SIZE 0x1c0000U

typedef struct {
    esp_iris_system_update_component_t descriptor;
    char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES];
    esp_partition_t target_partition;
    bool target_partition_valid;
    bool completed;
    bool image_verified;
} factory_update_plan_component_t;

typedef struct {
    bool prepared;
    factory_update_plan_component_t *plan;
    size_t plan_count;
    int active_index;
    uint32_t received;
    bool target_layout_valid;
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    uint8_t target_layout_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
    uint8_t *bootloader_image;
    uint8_t *partition_table_image;
    uint8_t *recovery_image;
    const esp_partition_t *active_partition;
    bool application_received;
    bool recovery_update;
    bool remote_bridge;
    bool preserve_layout;
    bool commit_started;
    bool defer_restart;
} factory_update_state_t;

typedef struct {
    factory_update_state_t transaction;
    factory_system_update_owner_t owner;
    factory_system_update_status_t status;
    portMUX_TYPE lock;
    bool layout_needs_reload;
} update_context_t;
#define TAG "factory_sysupdate"

/* All private stateful operations use the owning backend context. The public
 * facade and Iris callbacks are the only entrypoints into this transaction. */
bool update_owner_claim(update_context_t *ctx, factory_system_update_owner_t owner);
void update_owner_release(update_context_t *ctx);
bool update_owner_is(update_context_t *ctx, factory_system_update_owner_t owner);
void update_status_start(update_context_t *ctx, factory_system_update_owner_t owner,
                         const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                         size_t component_count, uint64_t total_size);
void update_status_reserve(update_context_t *ctx, factory_system_update_owner_t owner,
                           const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES]);
void update_status_component(update_context_t *ctx, uint8_t component_id, uint32_t received,
                             uint32_t size, esp_iris_system_update_phase_t phase);
void update_status_component_complete(update_context_t *ctx);
void update_status_finish(update_context_t *ctx, esp_iris_system_update_phase_t phase,
                          esp_err_t result);
bool update_bytes_equal(const uint8_t *left, const uint8_t *right, size_t size);
bool update_operation_id_valid(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES]);
esp_err_t update_decode_hex(const char *hex, uint8_t *output, size_t output_capacity,
                            size_t *output_size);
esp_err_t update_hash_flash_region(uint32_t address, size_t size, uint8_t output[32]);
esp_err_t update_hash_memory(const uint8_t *data, size_t size, uint8_t output[32]);
void update_state_reset(update_context_t *ctx);
const cJSON *update_json_member(const cJSON *object, const char *name);
esp_err_t update_json_string(const cJSON *object, const char *name, const char **value);
esp_err_t update_json_uint32(const cJSON *object, const char *name, uint32_t maximum,
                             uint32_t *value);
esp_err_t update_json_sha256(const cJSON *object, const char *name, uint8_t output[32]);
esp_err_t update_component_kind(const char *name, esp_iris_system_update_component_kind_t *kind);
esp_err_t update_authorize_component_target(const esp_iris_system_update_component_t *component);
esp_err_t update_parse_component(const cJSON *item, factory_update_plan_component_t *plan);
esp_err_t update_parse_manifest_root(update_context_t *ctx, const cJSON *root,
                                     uint8_t component_count, bool allow_bridge_bootloader);
esp_err_t update_parse_manifest_json(update_context_t *ctx,
                                     const esp_iris_system_update_manifest_t *manifest,
                                     bool allow_bridge_bootloader);
esp_err_t update_prepare_update_owned(update_context_t *ctx,
                                      const esp_iris_system_update_manifest_t *manifest,
                                      const cJSON *parsed_root, factory_system_update_owner_t owner,
                                      bool allow_bridge_bootloader);
esp_err_t update_prepare_update(const esp_iris_system_update_manifest_t *manifest, void *user_ctx);
int update_find_plan_component(update_context_t *ctx,
                               const esp_iris_system_update_component_t *component);
esp_err_t update_begin_component(const esp_iris_system_update_component_t *component,
                                 void *user_ctx);
esp_err_t update_write_component(const esp_iris_system_update_component_t *component,
                                 uint32_t offset, const uint8_t *data, size_t size, void *user_ctx);
esp_err_t update_validate_memory_image(const uint8_t *image, size_t size, const char *name);
esp_err_t update_validate_recovery_release(const uint8_t *image, size_t size);
const esp_partition_info_t *update_find_partition_entry(const esp_partition_info_t *entries,
                                                        int count, esp_partition_type_t type,
                                                        esp_partition_subtype_t subtype,
                                                        const char *label);
esp_err_t update_require_immutable_contract(const esp_partition_info_t *entries, int count);
bool update_partition_entry_is_immutable(const esp_partition_info_t *entry);
bool update_partition_entry_requires_remote_layout_image(const esp_partition_info_t *entry);
const esp_partition_info_t *
update_find_partition_entry_at_offset(const esp_partition_info_t *entries, int count,
                                      uint32_t offset);
void update_partition_descriptor_from_entry(const esp_partition_info_t *entry,
                                            esp_partition_t *partition);
esp_err_t update_resolve_component_targets(update_context_t *ctx,
                                           const esp_partition_info_t *entries, int count);
esp_err_t update_bridge_geometry(update_context_t *ctx, const esp_partition_info_t *entries,
                                 int count);
esp_err_t update_validate_partition_table(update_context_t *ctx, const uint8_t *image);
esp_err_t update_verify_application_on_flash(const esp_iris_system_update_component_t *component,
                                             const esp_partition_t *partition);
esp_err_t update_end_component(const esp_iris_system_update_component_t *component,
                               const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES],
                               void *user_ctx);
esp_err_t update_persist_result(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                                esp_err_t result);
esp_err_t update_commit_protected_image(const factory_update_plan_component_t *plan,
                                        const uint8_t *image);
const factory_update_plan_component_t *
update_plan_for_kind(update_context_t *ctx, esp_iris_system_update_component_kind_t kind);
void update_restart_task(void *arg);
esp_err_t update_commit_update(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                               void *user_ctx);
void update_abort_update_owned(update_context_t *ctx,
                               const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                               esp_err_t reason, factory_system_update_owner_t owner);
void update_abort_update(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                         esp_err_t reason, void *user_ctx);
esp_err_t update_source_prepare(update_context_t *ctx, factory_system_update_owner_t owner,
                                const uint8_t *manifest, size_t manifest_size,
                                const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                                bool allow_bridge_bootloader);
