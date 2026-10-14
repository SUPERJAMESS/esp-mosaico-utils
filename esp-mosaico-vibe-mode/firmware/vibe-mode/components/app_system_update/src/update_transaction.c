// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"

bool update_owner_claim(update_context_t *ctx, factory_system_update_owner_t owner)
{
    bool claimed = false;
    taskENTER_CRITICAL(&ctx->lock);
    if (ctx->owner == FACTORY_SYSTEM_UPDATE_OWNER_NONE && !ctx->layout_needs_reload) {
        ctx->owner = owner;
        claimed = true;
    }
    taskEXIT_CRITICAL(&ctx->lock);
    return claimed;
}

void update_owner_release(update_context_t *ctx)
{
    taskENTER_CRITICAL(&ctx->lock);
    ctx->owner = FACTORY_SYSTEM_UPDATE_OWNER_NONE;
    taskEXIT_CRITICAL(&ctx->lock);
}

bool update_owner_is(update_context_t *ctx, factory_system_update_owner_t owner)
{
    bool matches;
    taskENTER_CRITICAL(&ctx->lock);
    matches = ctx->owner == owner;
    taskEXIT_CRITICAL(&ctx->lock);
    return matches;
}

void update_status_start(update_context_t *ctx, factory_system_update_owner_t owner,
                         const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                         size_t component_count, uint64_t total_size)
{
    taskENTER_CRITICAL(&ctx->lock);
    memset(&ctx->status, 0, sizeof(ctx->status));
    ctx->status.owner = owner;
    memcpy(ctx->status.update.operation_id, operation_id, sizeof(ctx->status.update.operation_id));
    ctx->status.update.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED;
    ctx->status.update.component_count = (uint8_t)component_count;
    ctx->status.total_size = total_size;
    ctx->status.update.result = ESP_OK;
    taskEXIT_CRITICAL(&ctx->lock);
}

void update_status_reserve(update_context_t *ctx, factory_system_update_owner_t owner,
                           const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    taskENTER_CRITICAL(&ctx->lock);
    memset(&ctx->status, 0, sizeof(ctx->status));
    ctx->status.owner = owner;
    memcpy(ctx->status.update.operation_id, operation_id, sizeof(ctx->status.update.operation_id));
    ctx->status.update.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE;
    ctx->status.update.result = ESP_OK;
    taskEXIT_CRITICAL(&ctx->lock);
}

void update_status_component(update_context_t *ctx, uint8_t component_id, uint32_t received,
                             uint32_t size, esp_iris_system_update_phase_t phase)
{
    taskENTER_CRITICAL(&ctx->lock);
    ctx->status.update.active_component_id = component_id;
    ctx->status.update.component_received = received;
    ctx->status.received_size = ctx->status.completed_size + received;
    ctx->status.update.component_size = size;
    ctx->status.update.phase = phase;
    taskEXIT_CRITICAL(&ctx->lock);
}

void update_status_component_complete(update_context_t *ctx)
{
    taskENTER_CRITICAL(&ctx->lock);
    ++ctx->status.update.completed_components;
    ctx->status.completed_size = ctx->status.received_size;
    ctx->status.update.active_component_id = 0;
    ctx->status.update.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED;
    taskEXIT_CRITICAL(&ctx->lock);
}

void update_status_finish(update_context_t *ctx, esp_iris_system_update_phase_t phase,
                          esp_err_t result)
{
    taskENTER_CRITICAL(&ctx->lock);
    ctx->status.update.phase = phase;
    ctx->status.update.result = result;
    ctx->status.update.active_component_id = 0;
    taskEXIT_CRITICAL(&ctx->lock);
}

bool update_bytes_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

bool update_operation_id_valid(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES])
{
    if (operation_id == NULL) {
        return false;
    }
    uint8_t nonzero = 0;
    for (size_t i = 0; i < ESP_IRIS_SYSTEM_OPERATION_ID_BYTES; ++i) {
        nonzero |= operation_id[i];
    }
    return nonzero != 0;
}

void update_state_reset(update_context_t *ctx)
{
    free(ctx->transaction.bootloader_image);
    free(ctx->transaction.partition_table_image);
    free(ctx->transaction.recovery_image);
    ctx->transaction.bootloader_image = NULL;
    ctx->transaction.partition_table_image = NULL;
    ctx->transaction.recovery_image = NULL;
    ctx->transaction.prepared = false;
    ctx->transaction.plan_count = 0;
    ctx->transaction.active_index = -1;
    ctx->transaction.received = 0;
    ctx->transaction.target_layout_valid = false;
    ctx->transaction.remote_bridge = false;
    ctx->transaction.preserve_layout = false;
    ctx->transaction.commit_started = false;
    ctx->transaction.defer_restart = false;
    ctx->transaction.active_partition = NULL;
    ctx->transaction.application_received = false;
    ctx->transaction.recovery_update = false;
    memset(ctx->transaction.plan, 0,
           FACTORY_SYSTEM_MAX_COMPONENTS * sizeof(*ctx->transaction.plan));
    memset(ctx->transaction.operation_id, 0, sizeof(ctx->transaction.operation_id));
    memset(ctx->transaction.target_layout_sha256, 0, sizeof(ctx->transaction.target_layout_sha256));
}

esp_err_t update_prepare_update_owned(update_context_t *ctx,
                                      const esp_iris_system_update_manifest_t *manifest,
                                      const cJSON *parsed_root, factory_system_update_owner_t owner,
                                      bool allow_bridge_bootloader)
{
    if (manifest == NULL ||
        (parsed_root == NULL && (manifest->manifest == NULL || manifest->manifest_size == 0)) ||
        !update_operation_id_valid(manifest->operation_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool reserved = update_owner_is(ctx, owner);
    if (reserved) {
        ESP_RETURN_ON_FALSE(!ctx->transaction.prepared &&
                                update_bytes_equal(manifest->operation_id,
                                                   ctx->transaction.operation_id,
                                                   ESP_IRIS_SYSTEM_OPERATION_ID_BYTES),
                            ESP_ERR_INVALID_STATE, TAG, "system update reservation does not match");
    } else {
        ESP_RETURN_ON_FALSE(update_owner_claim(ctx, owner), ESP_ERR_INVALID_STATE, TAG,
                            "system update writer is busy");
        update_state_reset(ctx);
    }
    if (manifest->signature_size != 0) {
        update_status_finish(ctx, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED, ESP_ERR_INVALID_ARG);
        update_state_reset(ctx);
        update_owner_release(ctx);
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t parse_err =
        parsed_root != NULL
            ? update_parse_manifest_root(ctx, parsed_root, manifest->component_count,
                                         allow_bridge_bootloader)
            : update_parse_manifest_json(ctx, manifest, allow_bridge_bootloader);
    if (parse_err != ESP_OK) {
        update_status_finish(ctx, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED, parse_err);
        update_state_reset(ctx);
        update_owner_release(ctx);
        return parse_err;
    }
    memcpy(ctx->transaction.operation_id, manifest->operation_id,
           sizeof(ctx->transaction.operation_id));
    ctx->transaction.prepared = true;
    uint64_t total_size = 0;
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i)
        total_size += ctx->transaction.plan[i].descriptor.size;
    update_status_start(ctx, owner, manifest->operation_id, ctx->transaction.plan_count,
                        total_size);
    ESP_LOGW(TAG, "accepted unsigned system plan with %u component(s)",
             (unsigned)ctx->transaction.plan_count);
    return ESP_OK;
}

int update_find_plan_component(update_context_t *ctx,
                               const esp_iris_system_update_component_t *component)
{
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
        const esp_iris_system_update_component_t *expected = &ctx->transaction.plan[i].descriptor;
        if (expected->id == component->id && expected->kind == component->kind &&
            expected->flags == component->flags &&
            expected->target_offset == component->target_offset &&
            expected->size == component->size &&
            update_bytes_equal(expected->sha256, component->sha256, 32)) {
            return (int)i;
        }
    }
    return -1;
}

void update_abort_update_owned(update_context_t *ctx,
                               const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                               esp_err_t reason, factory_system_update_owner_t owner)
{
    if (!update_owner_is(ctx, owner)) {
        ESP_LOGW(TAG, "ignored abort from non-owner %d", (int)owner);
        return;
    }
    if (operation_id == NULL || !update_bytes_equal(operation_id, ctx->transaction.operation_id,
                                                    ESP_IRIS_SYSTEM_OPERATION_ID_BYTES)) {
        ESP_LOGW(TAG, "ignored abort for inactive operation");
        return;
    }
    const bool prepared = ctx->transaction.prepared;
    uint8_t saved_operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES] = {0};
    if (operation_id != NULL) {
        memcpy(saved_operation_id, operation_id, sizeof(saved_operation_id));
    }
    update_state_reset(ctx);
    update_status_finish(ctx, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED, reason);
    update_owner_release(ctx);
    if (prepared && update_persist_result(saved_operation_id, reason) != ESP_OK) {
        ESP_LOGE(TAG, "could not persist failed system-update result");
    }
    ESP_LOGW(TAG, "system update aborted: %s", esp_err_to_name(reason));
}

#endif
