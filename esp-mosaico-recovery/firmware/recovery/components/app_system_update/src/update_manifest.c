// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"

esp_err_t update_decode_hex(const char *hex, uint8_t *output, size_t output_capacity,
                            size_t *output_size)
{
    if (hex == NULL || output == NULL || output_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(hex);
    if (length == 0 || (length & 1U) != 0 || length / 2U > output_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < length / 2U; ++i) {
        uint8_t value = 0;
        for (size_t nibble = 0; nibble < 2; ++nibble) {
            const char character = hex[i * 2U + nibble];
            uint8_t digit;
            if (character >= '0' && character <= '9') {
                digit = (uint8_t)(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = (uint8_t)(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                digit = (uint8_t)(character - 'A' + 10);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            value = (uint8_t)((value << 4U) | digit);
        }
        output[i] = value;
    }
    *output_size = length / 2U;
    return ESP_OK;
}

const cJSON *update_json_member(const cJSON *object, const char *name)
{
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
}

esp_err_t update_json_string(const cJSON *object, const char *name, const char **value)
{
    const cJSON *item = update_json_member(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = item->valuestring;
    return ESP_OK;
}

esp_err_t update_json_uint32(const cJSON *object, const char *name, uint32_t maximum,
                             uint32_t *value)
{
    const cJSON *item = update_json_member(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t converted = (uint32_t)item->valuedouble;
    if ((double)converted != item->valuedouble) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = converted;
    return ESP_OK;
}

esp_err_t update_json_sha256(const cJSON *object, const char *name, uint8_t output[32])
{
    const char *hex = NULL;
    ESP_RETURN_ON_ERROR(update_json_string(object, name, &hex), TAG, "manifest %s", name);
    size_t output_size = 0;
    ESP_RETURN_ON_ERROR(update_decode_hex(hex, output, 32, &output_size), TAG, "manifest %s", name);
    return output_size == 32 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t update_component_kind(const char *name, esp_iris_system_update_component_kind_t *kind)
{
    if (strcmp(name, "application") == 0) {
        *kind = ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION;
    } else if (strcmp(name, "bootloader") == 0) {
        *kind = ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER;
    } else if (strcmp(name, "partition_table") == 0) {
        *kind = ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE;
    } else if (strcmp(name, "recovery") == 0) {
        *kind = ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY;
    } else if (strcmp(name, "data") == 0) {
        *kind = ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t update_parse_component(const cJSON *item, factory_update_plan_component_t *plan)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    const char *kind_name = NULL;
    const char *filename = NULL;
    ESP_RETURN_ON_ERROR(update_json_uint32(item, "id", UINT8_MAX, &id), TAG, "component id");
    ESP_RETURN_ON_FALSE(id != 0, ESP_ERR_INVALID_ARG, TAG, "component id is zero");
    ESP_RETURN_ON_ERROR(update_json_string(item, "kind", &kind_name), TAG, "component kind");
    const cJSON *flags_item = update_json_member(item, "flags");
    if (flags_item != NULL) {
        ESP_RETURN_ON_ERROR(update_json_uint32(item, "flags", UINT16_MAX, &flags), TAG,
                            "component flags");
    }
    uint32_t target_offset = 0;
    uint32_t size = 0;
    ESP_RETURN_ON_ERROR(update_json_uint32(item, "target_offset", UINT32_MAX, &target_offset), TAG,
                        "component target");
    ESP_RETURN_ON_ERROR(update_json_uint32(item, "size", UINT32_MAX, &size), TAG, "component size");
    ESP_RETURN_ON_FALSE(size != 0, ESP_ERR_INVALID_SIZE, TAG, "empty component");

    memset(plan, 0, sizeof(*plan));
    plan->descriptor.id = (uint8_t)id;
    plan->descriptor.flags = (uint16_t)flags;
    plan->descriptor.target_offset = target_offset;
    plan->descriptor.size = size;
    ESP_RETURN_ON_ERROR(update_component_kind(kind_name, &plan->descriptor.kind), TAG,
                        "component kind policy");
    ESP_RETURN_ON_ERROR(update_json_sha256(item, "sha256", plan->descriptor.sha256), TAG,
                        "component digest");
    ESP_RETURN_ON_ERROR(update_json_string(item, "file", &filename), TAG, "component file");
    const size_t filename_size = strnlen(filename, FACTORY_SYSTEM_UPDATE_FILENAME_BYTES);
    ESP_RETURN_ON_FALSE(filename_size > 0 && filename_size < FACTORY_SYSTEM_UPDATE_FILENAME_BYTES &&
                            strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0 &&
                            strchr(filename, '/') == NULL && strchr(filename, '\\') == NULL,
                        ESP_ERR_INVALID_ARG, TAG, "component file is not root-level");
    strlcpy(plan->filename, filename, sizeof(plan->filename));
    return update_authorize_component_target(&plan->descriptor);
}

esp_err_t update_parse_manifest_root(update_context_t *ctx, const cJSON *root,
                                     uint8_t component_count, bool allow_bridge_bootloader)
{
    esp_err_t err = ESP_OK;
    const char *schema = NULL;
    uint32_t chip_id = 0;
    uint32_t flash_size = 0;
    uint32_t actual_flash_size = 0;
    const cJSON *target = update_json_member(root, "target");
    const cJSON *components = update_json_member(root, "components");
    const cJSON *minimum_recovery = update_json_member(root, "minimum_recovery_version");

    if (update_json_string(root, "schema", &schema) != ESP_OK ||
        strcmp(schema, FACTORY_SYSTEM_SCHEMA) != 0 ||
        update_json_member(root, "source_layout_sha256") != NULL ||
        update_json_member(root, "signature") != NULL ||
        update_json_uint32(target, "chip_id", UINT16_MAX, &chip_id) != ESP_OK ||
        chip_id != FACTORY_SYSTEM_ESP32S31_CHIP_ID ||
        update_json_uint32(target, "flash_size", UINT32_MAX, &flash_size) != ESP_OK ||
        esp_flash_get_size(NULL, &actual_flash_size) != ESP_OK || flash_size != actual_flash_size ||
        !cJSON_IsArray(components) || cJSON_GetArraySize(components) != component_count ||
        component_count == 0 || component_count > FACTORY_SYSTEM_MAX_COMPONENTS) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (minimum_recovery != NULL &&
        (!cJSON_IsString(minimum_recovery) ||
         !factory_recovery_version_satisfies(esp_app_get_description()->version,
                                             minimum_recovery->valuestring))) {
        err = ESP_ERR_INVALID_VERSION;
        goto done;
    }
    err = update_json_sha256(root, "target_layout_sha256", ctx->transaction.target_layout_sha256);
    if (err != ESP_OK) {
        goto done;
    }

    /* Only the product Bridge source may select the remote policy. */
    ctx->transaction.remote_bridge = update_owner_is(ctx, FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE);
    if (update_json_member(root, "remote_bridge") != NULL) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    ctx->transaction.preserve_layout = cJSON_IsTrue(update_json_member(root, "preserve_layout"));
    if (ctx->transaction.preserve_layout && !ctx->transaction.remote_bridge) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    bool seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA + 1] = {false};
    const cJSON *component = NULL;
    cJSON_ArrayForEach(component, components)
    {
        factory_update_plan_component_t *plan = &ctx->transaction.plan[ctx->transaction.plan_count];
        err = update_parse_component(component, plan);
        if (err != ESP_OK ||
            (plan->descriptor.kind != ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA &&
             !(ctx->transaction.remote_bridge &&
               plan->descriptor.kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) &&
             seen_kinds[plan->descriptor.kind])) {
            err = err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
            goto done;
        }
        if (ctx->transaction.remote_bridge && !allow_bridge_bootloader &&
            plan->descriptor.kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
            err = ESP_ERR_NOT_SUPPORTED;
            goto done;
        }
        for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
            const esp_iris_system_update_component_t *previous =
                &ctx->transaction.plan[i].descriptor;
            const uint64_t previous_end = (uint64_t)previous->target_offset + previous->size;
            const uint64_t current_end =
                (uint64_t)plan->descriptor.target_offset + plan->descriptor.size;
            if (previous->id == plan->descriptor.id ||
                strcmp(ctx->transaction.plan[i].filename, plan->filename) == 0 ||
                ((uint64_t)plan->descriptor.target_offset < previous_end &&
                 (uint64_t)previous->target_offset < current_end)) {
                err = ESP_ERR_INVALID_ARG;
                goto done;
            }
        }
        seen_kinds[plan->descriptor.kind] = true;
        ++ctx->transaction.plan_count;
    }
    /* Bootloader replacement always carries its matching partition table. */
    if (seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER] &&
        !seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE]) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    ctx->transaction.recovery_update = seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY];
    if (ctx->transaction.recovery_update) {
        if (ctx->transaction.plan_count != 1U ||
            ctx->transaction.plan[0].descriptor.kind != ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
            ESP_LOGE(TAG, "recovery update must contain only one recovery "
                          "component");
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
    } else if (!ctx->transaction.preserve_layout &&
               (!seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE] ||
                ctx->transaction.plan[0].descriptor.kind !=
                    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE)) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
        if (ctx->transaction.plan[i].descriptor.kind ==
                ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE &&
            !update_bytes_equal(ctx->transaction.plan[i].descriptor.sha256,
                                ctx->transaction.target_layout_sha256, 32)) {
            err = ESP_ERR_INVALID_CRC;
            goto done;
        }
    }
    if (ctx->transaction.preserve_layout) {
        if (seen_kinds[ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE]) {
            err = ESP_ERR_INVALID_ARG;
            goto done;
        }
        uint8_t *current = malloc(FACTORY_SYSTEM_FLASH_SECTOR_BYTES);
        uint8_t digest[32];
        if (current == NULL) {
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        err = update_hash_flash_region(CONFIG_PARTITION_TABLE_OFFSET,
                                       FACTORY_SYSTEM_FLASH_SECTOR_BYTES, digest);
        if (err == ESP_OK && !update_bytes_equal(digest, ctx->transaction.target_layout_sha256, 32))
            err = ESP_ERR_INVALID_VERSION;
        if (err == ESP_OK)
            err = esp_flash_read(NULL, current, CONFIG_PARTITION_TABLE_OFFSET,
                                 FACTORY_SYSTEM_FLASH_SECTOR_BYTES);
        if (err == ESP_OK)
            err = update_validate_partition_table(ctx, current);
        free(current);
        if (err != ESP_OK)
            goto done;
    }
    if (ctx->transaction.recovery_update) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running == NULL || running->type != ESP_PARTITION_TYPE_APP ||
            running->subtype != ESP_PARTITION_SUBTYPE_APP_TEST ||
            running->address != FACTORY_SYSTEM_RECOVERY_OFFSET ||
            running->size != FACTORY_SYSTEM_RECOVERY_SIZE) {
            ESP_LOGE(TAG, "recovery self-update requires the running Vibe Mode test slot");
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }
        uint8_t current_layout_sha256[32];
        err = update_hash_flash_region(CONFIG_PARTITION_TABLE_OFFSET,
                                       FACTORY_SYSTEM_FLASH_SECTOR_BYTES, current_layout_sha256);
        if (err != ESP_OK ||
            !update_bytes_equal(current_layout_sha256, ctx->transaction.target_layout_sha256, 32)) {
            ESP_LOGE(TAG, "recovery source layout does not match manifest");
            err = err == ESP_OK ? ESP_ERR_INVALID_VERSION : err;
            goto done;
        }
        ctx->transaction.target_layout_valid = true;
    }

done:
    return err;
}

esp_err_t update_parse_manifest_json(update_context_t *ctx,
                                     const esp_iris_system_update_manifest_t *manifest,
                                     bool allow_bridge_bootloader)
{
    char *json = malloc(manifest->manifest_size + 1U);
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, TAG, "allocate manifest");
    memcpy(json, manifest->manifest, manifest->manifest_size);
    json[manifest->manifest_size] = '\0';
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, manifest->manifest_size + 1U, &parse_end, true);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (root != NULL && parse_end == json + manifest->manifest_size) {
        err = update_parse_manifest_root(ctx, root, manifest->component_count,
                                         allow_bridge_bootloader);
    }
    cJSON_Delete(root);
    free(json);
    return err;
}

#endif
