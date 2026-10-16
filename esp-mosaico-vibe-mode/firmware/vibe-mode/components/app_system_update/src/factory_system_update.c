// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#include <string.h>
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"

/* Large plans stay in zero-filled PSRAM. The lock and control context stay in
 * internal RAM; an initialized aggregate containing the plan would grow BIN. */
static EXT_RAM_BSS_ATTR factory_update_plan_component_t
    s_plan_storage[FACTORY_SYSTEM_MAX_COMPONENTS];
static update_context_t s_backend = {
    .transaction = {.plan = s_plan_storage, .active_index = -1},
    .owner = FACTORY_SYSTEM_UPDATE_OWNER_NONE,
    .status = {.owner = FACTORY_SYSTEM_UPDATE_OWNER_NONE,
               .update = {.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE, .result = ESP_OK}},
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

bool factory_system_update_writer_busy(void)
{
    update_context_t *ctx = &s_backend;
    taskENTER_CRITICAL(&ctx->lock);
    bool busy = ctx->owner != FACTORY_SYSTEM_UPDATE_OWNER_NONE || ctx->layout_needs_reload;
    taskEXIT_CRITICAL(&ctx->lock);
    return busy;
}
esp_err_t factory_system_update_read_acquire(void)
{
    if (!update_owner_claim(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_READER))
        return ESP_ERR_INVALID_STATE;
    esp_iris_ota_status_t ota = {0};
    esp_err_t err = esp_iris_ota_get_status(&ota);
    /* A lazy/unconfigured OTA service owns no writer yet. The claimed owner
     * prevents a later BEGIN from selecting its target. */
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) err = ESP_OK;
    if (err != ESP_OK || ota.active) {
        update_owner_release(&s_backend);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
void factory_system_update_read_release(void)
{
    if (update_owner_is(&s_backend, FACTORY_SYSTEM_UPDATE_OWNER_READER))
        update_owner_release(&s_backend);
}

esp_err_t factory_system_update_get_status(factory_system_update_status_t *status)
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status output is null");
    taskENTER_CRITICAL(&ctx->lock);
    *status = ctx->status;
    taskEXIT_CRITICAL(&ctx->lock);
    return ESP_OK;
}

esp_err_t
factory_system_update_source_reserve(factory_system_update_owner_t owner,
                                     const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE((owner == FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE ||
                         owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND ||
                         owner == FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE) &&
                            update_operation_id_valid(operation_id),
                        ESP_ERR_INVALID_ARG, TAG, "invalid system-update reservation");
    ESP_RETURN_ON_FALSE(update_owner_claim(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                        "system update writer is busy");
    update_state_reset(ctx);
    memcpy(ctx->transaction.operation_id, operation_id, sizeof(ctx->transaction.operation_id));
    update_status_reserve(ctx, owner, operation_id);
    return ESP_OK;
}

esp_err_t factory_system_update_source_reserve_idea(const uint8_t operation_id[16])
{
    update_context_t *ctx = &s_backend;
    if (!update_operation_id_valid(operation_id))
        return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&ctx->lock);
    bool admitted =
        ctx->owner == FACTORY_SYSTEM_UPDATE_OWNER_NONE ||
        (ctx->owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND && ctx->transaction.defer_restart &&
         ctx->status.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED);
    if (admitted)
        ctx->owner = FACTORY_SYSTEM_UPDATE_OWNER_NAND;
    taskEXIT_CRITICAL(&ctx->lock);
    if (!admitted)
        return ESP_ERR_INVALID_STATE;
    update_state_reset(ctx);
    memcpy(ctx->transaction.operation_id, operation_id, sizeof(ctx->transaction.operation_id));
    update_status_reserve(ctx, FACTORY_SYSTEM_UPDATE_OWNER_NAND, operation_id);
    return ESP_OK;
}

esp_err_t update_source_prepare(update_context_t *ctx, factory_system_update_owner_t owner,
                                const uint8_t *manifest, size_t manifest_size,
                                const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                                bool allow_bridge_bootloader)
{
    ESP_RETURN_ON_FALSE(manifest != NULL && manifest_size > 0 &&
                            manifest_size <= FACTORY_SOURCE_MANIFEST_BYTES && operation_id != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid local update manifest");

    ESP_RETURN_ON_FALSE(update_operation_id_valid(operation_id), ESP_ERR_INVALID_ARG, TAG,
                        "local operation ID is zero");

    const char *parse_end = NULL;
    cJSON *root =
        cJSON_ParseWithLengthOpts((const char *)manifest, manifest_size, &parse_end, false);
    /* JSON permits trailing SP, HT, LF and CR. Keep the original bytes for
     * integrity checks, and reject any other bytes after the parsed value. */
    const char *end = (const char *)manifest + manifest_size;
    while (root != NULL && parse_end < end &&
           (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\n' || *parse_end == '\r')) {
        ++parse_end;
    }
    if (root == NULL || parse_end != end) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *components = update_json_member(root, "components");
    const int component_count = cJSON_IsArray(components) ? cJSON_GetArraySize(components) : 0;
    if (component_count <= 0 || component_count > FACTORY_SYSTEM_MAX_COMPONENTS) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_iris_system_update_manifest_t descriptor = {
        .manifest = manifest,
        .manifest_size = manifest_size,
        .signature = NULL,
        .signature_size = 0,
        .component_count = (uint8_t)component_count,
    };
    memcpy(descriptor.operation_id, operation_id, sizeof(descriptor.operation_id));
    if (owner != FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE && owner != FACTORY_SYSTEM_UPDATE_OWNER_NAND &&
        owner != FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err =
        update_prepare_update_owned(ctx, &descriptor, root, owner, allow_bridge_bootloader);
    cJSON_Delete(root);
    return err;
}

esp_err_t
factory_system_update_source_prepare(factory_system_update_owner_t owner, const uint8_t *manifest,
                                     size_t manifest_size,
                                     const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    update_context_t *ctx = &s_backend;
    return update_source_prepare(ctx, owner, manifest, manifest_size, operation_id, false);
}

esp_err_t factory_system_update_source_prepare_bridge(
    const cJSON *manifest, const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
    bool allow_bootloader)
{
    update_context_t *ctx = &s_backend;
    /* Local product policy, never populated from manifest JSON. */
    const cJSON *components = update_json_member(manifest, "components");
    const int component_count = cJSON_IsArray(components) ? cJSON_GetArraySize(components) : 0;
    ESP_RETURN_ON_FALSE(component_count > 0 && component_count <= FACTORY_SYSTEM_MAX_COMPONENTS &&
                            operation_id != NULL,
                        ESP_ERR_INVALID_SIZE, TAG, "Bridge component count");
    esp_iris_system_update_manifest_t descriptor = {
        .component_count = (uint8_t)component_count,
    };
    memcpy(descriptor.operation_id, operation_id, sizeof(descriptor.operation_id));
    return update_prepare_update_owned(ctx, &descriptor, manifest,
                                       FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, allow_bootloader);
}

size_t factory_system_update_source_component_count(factory_system_update_owner_t owner)
{
    update_context_t *ctx = &s_backend;
    return update_owner_is(ctx, owner) && ctx->transaction.prepared ? ctx->transaction.plan_count
                                                                    : 0;
}

esp_err_t
factory_system_update_source_component(factory_system_update_owner_t owner, size_t index,
                                       esp_iris_system_update_component_t *component,
                                       char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES])
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, owner) && ctx->transaction.prepared &&
                            index < ctx->transaction.plan_count && component != NULL &&
                            filename != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid local component request");
    *component = ctx->transaction.plan[index].descriptor;
    strlcpy(filename, ctx->transaction.plan[index].filename, FACTORY_SYSTEM_UPDATE_FILENAME_BYTES);
    return ESP_OK;
}

esp_err_t
factory_system_update_source_begin_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component)
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                        "update source does not own writer");
    return update_begin_component(component, ctx);
}

esp_err_t
factory_system_update_source_write_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component,
                                             uint32_t offset, const uint8_t *data, size_t size)
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                        "update source does not own writer");
    return update_write_component(component, offset, data, size, ctx);
}

esp_err_t factory_system_update_source_end_component(
    factory_system_update_owner_t owner, const esp_iris_system_update_component_t *component,
    const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES])
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                        "update source does not own writer");
    return update_end_component(component, actual_sha256, ctx);
}

esp_err_t factory_system_update_source_end_idea_component(
    const esp_iris_system_update_component_t *component)
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, FACTORY_SYSTEM_UPDATE_OWNER_NAND) &&
                            ctx->transaction.prepared && !ctx->transaction.remote_bridge &&
                            !ctx->transaction.recovery_update && component != NULL &&
                            update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) == NULL &&
                            ctx->transaction.active_index >= 0 &&
                            ctx->transaction.received == component->size,
                        ESP_ERR_INVALID_STATE, TAG, "Idea component end state");
    int index = update_find_plan_component(ctx, component);
    ESP_RETURN_ON_FALSE(index == ctx->transaction.active_index, ESP_ERR_INVALID_ARG, TAG,
                        "Idea descriptor does not match plan");
    ESP_RETURN_ON_FALSE(component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION ||
                            component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA ||
                            component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE,
                        ESP_ERR_NOT_SUPPORTED, TAG, "not an ordinary Idea component");
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
        uint8_t digest[32];
        ESP_RETURN_ON_ERROR(update_hash_memory(ctx->transaction.partition_table_image,
                                               component->size, digest), TAG, "hash staged layout");
        ESP_RETURN_ON_FALSE(update_bytes_equal(digest, component->sha256, sizeof(digest)),
                            ESP_ERR_INVALID_CRC, TAG, "staged layout digest mismatch");
    }
    /* The normal backend compares actual NOR readback against this expected
     * digest and validates the application image before marking it complete. */
    return update_end_component(component, component->sha256, ctx);
}

bool factory_system_update_source_needs_restart(factory_system_update_owner_t owner)
{
    update_context_t *ctx = &s_backend;
    return update_owner_is(ctx, owner) && ctx->transaction.remote_bridge &&
           ctx->transaction.commit_started;
}

esp_err_t
factory_system_update_source_commit(factory_system_update_owner_t owner,
                                    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    update_context_t *ctx = &s_backend;
    ESP_RETURN_ON_FALSE(update_owner_is(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                        "update source does not own writer");
    return update_commit_update(operation_id, ctx);
}

esp_err_t factory_system_update_source_commit_deferred(const uint8_t operation_id[16])
{
    update_context_t *ctx = &s_backend;
    if (!update_owner_is(ctx, FACTORY_SYSTEM_UPDATE_OWNER_NAND) || ctx->transaction.remote_bridge ||
        ctx->transaction.recovery_update ||
        update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) != NULL)
        return ESP_ERR_INVALID_STATE;
    ctx->transaction.defer_restart = true;
    return update_commit_update(operation_id, ctx);
}

void factory_system_update_source_abort(
    factory_system_update_owner_t owner,
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES], esp_err_t reason)
{
    update_context_t *ctx = &s_backend;
    update_abort_update_owned(ctx, operation_id, reason, owner);
}

esp_err_t factory_system_update_register(void)
{
    update_context_t *ctx = &s_backend;
    const esp_iris_system_update_backend_t backend = {
        .prepare = update_prepare_update,
        .begin_component = update_begin_component,
        .write_component = update_write_component,
        .end_component = update_end_component,
        .commit = update_commit_update,
        .abort = update_abort_update,
        .user_ctx = ctx,
    };
    ESP_RETURN_ON_ERROR(esp_iris_system_update_register(&backend), TAG,
                        "register system update backend");
    ESP_LOGW(TAG, "unsigned full-system update backend enabled");
    return ESP_OK;
}

#else
esp_err_t factory_system_update_read_acquire(void) { return ESP_ERR_NOT_SUPPORTED; }
void factory_system_update_read_release(void) {}

bool factory_system_update_writer_busy(void)
{
    return false;
}
esp_err_t factory_system_update_source_reserve_idea(const uint8_t operation_id[16])
{
    (void)operation_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_source_commit_deferred(const uint8_t operation_id[16])
{
    (void)operation_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_register(void)
{
    return ESP_OK;
}

esp_err_t factory_system_update_get_status(factory_system_update_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->update.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE;
    return ESP_OK;
}

esp_err_t
factory_system_update_source_reserve(factory_system_update_owner_t owner,
                                     const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    (void)owner;
    (void)operation_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t
factory_system_update_source_prepare(factory_system_update_owner_t owner, const uint8_t *manifest,
                                     size_t manifest_size,
                                     const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    (void)owner;
    (void)manifest;
    (void)manifest_size;
    (void)operation_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_source_prepare_bridge(
    const cJSON *manifest, const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
    bool allow_bootloader)
{
    (void)manifest;
    (void)operation_id;
    (void)allow_bootloader;
    return ESP_ERR_NOT_SUPPORTED;
}

size_t factory_system_update_source_component_count(factory_system_update_owner_t owner)
{
    (void)owner;
    return 0;
}

esp_err_t
factory_system_update_source_component(factory_system_update_owner_t owner, size_t index,
                                       esp_iris_system_update_component_t *component,
                                       char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES])
{
    (void)owner;
    (void)index;
    (void)component;
    (void)filename;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t
factory_system_update_source_begin_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component)
{
    (void)owner;
    (void)component;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t
factory_system_update_source_write_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component,
                                             uint32_t offset, const uint8_t *data, size_t size)
{
    (void)owner;
    (void)component;
    (void)offset;
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_source_end_component(
    factory_system_update_owner_t owner, const esp_iris_system_update_component_t *component,
    const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES])
{
    (void)owner;
    (void)component;
    (void)actual_sha256;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t factory_system_update_source_end_idea_component(
    const esp_iris_system_update_component_t *component)
{
    (void)component;
    return ESP_ERR_NOT_SUPPORTED;
}

bool factory_system_update_source_needs_restart(factory_system_update_owner_t owner)
{
    (void)owner;
    return false;
}

esp_err_t
factory_system_update_source_commit(factory_system_update_owner_t owner,
                                    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    (void)owner;
    (void)operation_id;
    return ESP_ERR_NOT_SUPPORTED;
}

void factory_system_update_source_abort(
    factory_system_update_owner_t owner,
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES], esp_err_t reason)
{
    (void)owner;
    (void)operation_id;
    (void)reason;
}

#endif
