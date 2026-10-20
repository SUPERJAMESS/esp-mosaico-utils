// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"
#include "factory_io_metrics.h"

esp_err_t update_hash_flash_region(uint32_t address, size_t size, uint8_t output[32])
{
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    uint8_t buffer[FACTORY_SYSTEM_HASH_CHUNK_BYTES];
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    for (size_t offset = 0; offset < size;) {
        size_t chunk = size - offset;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        int64_t started = factory_io_now();
        err = esp_flash_read(NULL, buffer, address + offset, chunk);
        metrics.read += factory_io_now() - started;
        started = factory_io_now();
        bool hash_ok = err == ESP_OK && psa_hash_update(&operation, buffer, chunk) == PSA_SUCCESS;
        metrics.hash += factory_io_now() - started;
        if (err != ESP_OK || !hash_ok) {
            err = err == ESP_OK ? ESP_FAIL : err;
            break;
        }
        offset += chunk;
        metrics.bytes += chunk;
    }
    size_t actual_size = 0;
    if (err == ESP_OK && (psa_hash_finish(&operation, output, 32, &actual_size) != PSA_SUCCESS ||
                          actual_size != 32)) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        (void)psa_hash_abort(&operation);
    }
    char region[16];
    snprintf(region, sizeof(region), "0x%08" PRIx32, address);
    factory_io_report("nor_hash", region, &metrics, buffer, sizeof(buffer), err);
    return err;
}

esp_err_t update_hash_memory(const uint8_t *data, size_t size, uint8_t output[32])
{
    size_t output_size = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
                   psa_hash_compute(PSA_ALG_SHA_256, data, size, output, 32, &output_size) ==
                       PSA_SUCCESS &&
                   output_size == 32
               ? ESP_OK
               : ESP_FAIL;
}

/* Keep 64 KiB block alignment so the SDK can use block erase. Chunking at
 * sector size would make large images substantially slower. Raw reads are
 * required: decrypted 0xff data does not establish that NOR is erased. */
static esp_err_t update_erase_partition(update_context_t *ctx,
                                         const esp_iris_system_update_component_t *component,
                                         size_t size)
{
    const esp_partition_t *partition = ctx->transaction.active_partition;
    update_status_component(ctx, component->id, 0, component->size,
                            ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED);
    taskENTER_CRITICAL(&ctx->lock);
    ctx->status.erasing = true;
    ctx->status.erase_done = 0;
    ctx->status.erase_total = (uint32_t)size;
    taskEXIT_CRITICAL(&ctx->lock);
    const int64_t started = factory_io_now();
    size_t skipped = 0;
    esp_err_t err = ESP_OK;
    uint8_t buffer[FACTORY_SYSTEM_HASH_CHUNK_BYTES];
    for (size_t offset = 0; offset < size;) {
        size_t chunk = FACTORY_SYSTEM_ERASE_CHUNK_BYTES -
            (partition->address + offset) % FACTORY_SYSTEM_ERASE_CHUNK_BYTES;
        if (chunk > size - offset) chunk = size - offset;
        bool blank = true;
        for (size_t checked = 0; blank && checked < chunk;) {
            size_t count = chunk - checked;
            if (count > sizeof(buffer)) count = sizeof(buffer);
            err = esp_partition_read_raw(partition, offset + checked, buffer, count);
            if (err != ESP_OK) break;
            for (size_t i = 0; i < count; ++i) {
                if (buffer[i] != 0xff) { blank = false; break; }
            }
            checked += count;
        }
        if (err != ESP_OK) break;
        if (blank) skipped += chunk;
        else err = esp_partition_erase_range(partition, offset, chunk);
        if (err != ESP_OK) break;
        offset += chunk;
        taskENTER_CRITICAL(&ctx->lock);
        ctx->status.erase_done = (uint32_t)offset;
        taskEXIT_CRITICAL(&ctx->lock);
    }
    taskENTER_CRITICAL(&ctx->lock);
    ctx->status.erasing = false;
    taskEXIT_CRITICAL(&ctx->lock);
    ESP_LOGI(TAG, "preparation complete: bytes=%u blank_skipped=%u elapsed_us=%lld result=0x%x",
             (unsigned)size, (unsigned)skipped, (long long)(factory_io_now() - started), (unsigned)err);
    return err;
}

esp_err_t update_begin_component(const esp_iris_system_update_component_t *component,
                                 void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    ESP_RETURN_ON_FALSE(component != NULL && ctx->transaction.prepared &&
                            ctx->transaction.active_index < 0,
                        ESP_ERR_INVALID_STATE, TAG, "component begin state");
    const int index = update_find_plan_component(ctx, component);
    ESP_RETURN_ON_FALSE(index >= 0 && !ctx->transaction.plan[index].completed, ESP_ERR_INVALID_ARG,
                        TAG, "descriptor not in validated plan");
    factory_update_plan_component_t *plan = &ctx->transaction.plan[index];

    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) {
        ESP_RETURN_ON_FALSE(plan->target_partition_valid && ctx->transaction.target_layout_valid,
                            ESP_ERR_NOT_FOUND, TAG, "application target missing");
        ctx->transaction.active_partition = &plan->target_partition;
        const size_t erase_size = (component->size + FACTORY_SYSTEM_FLASH_SECTOR_BYTES - 1U) &
                                  ~(FACTORY_SYSTEM_FLASH_SECTOR_BYTES - 1U);
        ESP_LOGI(TAG, "erasing application before receive: offset=0x%08" PRIx32 " size=%u",
                 ctx->transaction.active_partition->address, (unsigned)erase_size);
        ESP_RETURN_ON_ERROR(
            update_erase_partition(ctx, component, erase_size), TAG,
            "erase application");
        ESP_LOGI(TAG, "application erase complete");
    } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
        ESP_RETURN_ON_FALSE(plan->target_partition_valid && ctx->transaction.target_layout_valid,
                            ESP_ERR_NOT_FOUND, TAG, "data partition target missing");
        ctx->transaction.active_partition = &plan->target_partition;
        ESP_LOGI(TAG,
                 "erasing data partition before receive: label=%s "
                 "offset=0x%08" PRIx32 " size=%" PRIu32,
                 ctx->transaction.active_partition->label,
                 ctx->transaction.active_partition->address,
                 ctx->transaction.active_partition->size);
        ESP_RETURN_ON_ERROR(update_erase_partition(ctx, component, ctx->transaction.active_partition->size),
                            TAG, "erase data partition");
        ESP_LOGI(TAG, "data partition erase complete: label=%s",
                 ctx->transaction.active_partition->label);
    } else {
        uint8_t **destination = NULL;
        if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
            destination = &ctx->transaction.bootloader_image;
        } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
            destination = &ctx->transaction.partition_table_image;
        } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
            destination = &ctx->transaction.recovery_image;
        }
        ESP_RETURN_ON_FALSE(destination != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                            "unsupported staged component");
        free(*destination);
        *destination = heap_caps_malloc(component->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(*destination != NULL, ESP_ERR_NO_MEM, TAG, "stage protected component");
        memset(*destination, 0xff, component->size);
    }
    ctx->transaction.active_index = index;
    ctx->transaction.received = 0;
    update_status_component(ctx, component->id, 0, component->size,
                            ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING);
    return ESP_OK;
}

esp_err_t update_write_component(const esp_iris_system_update_component_t *component,
                                 uint32_t offset, const uint8_t *data, size_t size, void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    if (component == NULL || data == NULL || size == 0 || ctx->transaction.active_index < 0 ||
        offset != ctx->transaction.received) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (update_find_plan_component(ctx, component) != ctx->transaction.active_index)
        return ESP_ERR_INVALID_ARG;
    /* Allocation and target permissions came from the accepted plan. Never
     * use a caller's changed size/kind/offset to write or validate a component. */
    component = &ctx->transaction.plan[ctx->transaction.active_index].descriptor;
    if (offset > component->size || size > component->size - offset)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = ESP_OK;
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) {
        err = ctx->transaction.active_partition != NULL
                  ? esp_partition_write(ctx->transaction.active_partition, offset, data, size)
                  : ESP_ERR_INVALID_STATE;
    } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
        err = ctx->transaction.active_partition != NULL
                  ? esp_partition_write(ctx->transaction.active_partition, offset, data, size)
                  : ESP_ERR_INVALID_STATE;
    } else {
        uint8_t *destination = NULL;
        if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
            destination = ctx->transaction.bootloader_image;
        } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
            destination = ctx->transaction.partition_table_image;
        } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
            destination = ctx->transaction.recovery_image;
        }
        if (destination == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(destination + offset, data, size);
    }
    if (err == ESP_OK) {
        ctx->transaction.received += (uint32_t)size;
        update_status_component(ctx, component->id, ctx->transaction.received, component->size,
                                ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING);
    }
    return err;
}

esp_err_t update_end_component(const esp_iris_system_update_component_t *component,
                               const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES],
                               void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    ESP_RETURN_ON_FALSE(component != NULL && actual_sha256 != NULL &&
                            ctx->transaction.active_index >= 0,
                        ESP_ERR_INVALID_STATE, TAG, "component end state");
    ESP_RETURN_ON_FALSE(update_find_plan_component(ctx, component) == ctx->transaction.active_index,
                        ESP_ERR_INVALID_ARG, TAG, "component descriptor does not match plan");
    factory_update_plan_component_t *plan = &ctx->transaction.plan[ctx->transaction.active_index];
    component = &plan->descriptor;
    ESP_RETURN_ON_FALSE(ctx->transaction.received == component->size,
                        ESP_ERR_INVALID_STATE, TAG, "component end size");
    ESP_RETURN_ON_FALSE(update_bytes_equal(actual_sha256, plan->descriptor.sha256, 32),
                        ESP_ERR_INVALID_CRC, TAG, "component digest does not match plan");

    esp_err_t ret = ESP_OK;
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION ||
        component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
        uint8_t readback_sha256[32];
        ESP_LOGI(TAG, "verifying %s readback SHA-256",
                 component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION
                     ? ctx->transaction.active_partition->label
                     : ctx->transaction.active_partition->label);
        ESP_GOTO_ON_ERROR(
            update_hash_flash_region(component->target_offset, component->size, readback_sha256),
            done, TAG, "read back component");
        ESP_GOTO_ON_FALSE(update_bytes_equal(readback_sha256, actual_sha256, 32),
                          ESP_ERR_INVALID_CRC, done, TAG, "component readback mismatch");
        if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION) {
            factory_io_metrics_t metrics = {.started = factory_io_now(), .bytes = component->size};
            ret = update_verify_application_on_flash(component, ctx->transaction.active_partition);
            factory_io_report("nor_image", "application", &metrics, NULL, 0, ret);
            ESP_GOTO_ON_ERROR(ret, done, TAG, "application image validation");
            plan->image_verified = true;
            ctx->transaction.application_received = true;
            ESP_LOGI(TAG, "application image and readback verified");
        } else {
            ESP_LOGI(TAG, "data image and readback verified: label=%s",
                     ctx->transaction.active_partition->label);
        }
    } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
        ESP_GOTO_ON_ERROR(update_validate_memory_image(ctx->transaction.bootloader_image,
                                                       component->size, "bootloader"),
                          done, TAG, "bootloader validation");
    } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
        ESP_GOTO_ON_ERROR(update_validate_memory_image(ctx->transaction.recovery_image,
                                                       component->size, "recovery"),
                          done, TAG, "recovery validation");
        ESP_GOTO_ON_ERROR(
            update_validate_recovery_release(ctx->transaction.recovery_image, component->size),
            done, TAG, "recovery release validation");
    } else if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
        ESP_GOTO_ON_ERROR(
            update_validate_partition_table(ctx, ctx->transaction.partition_table_image), done, TAG,
            "immutable partition policy");
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    plan->completed = true;
    update_status_component_complete(ctx);
done:
    ctx->transaction.active_partition = NULL;
    ctx->transaction.active_index = -1;
    ctx->transaction.received = 0;
    return ret;
}

esp_err_t update_persist_result(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                                esp_err_t result)
{
    return factory_system_metadata_store_last_result(operation_id, result);
}

esp_err_t update_commit_protected_image(update_context_t *ctx, const factory_update_plan_component_t *plan, const uint8_t *image)
{
    if (plan == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(image != NULL && plan->completed, ESP_ERR_INVALID_STATE, TAG,
                        "protected image is incomplete");
    const esp_iris_system_update_component_t *component = &plan->descriptor;
    bool recovery = component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY;
    if (recovery) {
        taskENTER_CRITICAL(&ctx->lock);
        ctx->status.commit_written = 0;
        ctx->status.commit_total = component->size;
        taskEXIT_CRITICAL(&ctx->lock);
    }
    uint8_t digest[32];
    ESP_RETURN_ON_ERROR(update_hash_flash_region(component->target_offset, component->size, digest),
                        TAG, "hash current protected component");
    if (update_bytes_equal(digest, component->sha256, 32)) {
        if (recovery) {
            taskENTER_CRITICAL(&ctx->lock);
            ctx->status.commit_written = component->size;
            taskEXIT_CRITICAL(&ctx->lock);
        }
        ESP_LOGI(TAG, "protected component already matches; skipping write: id=%u", component->id);
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "temporarily disabling dangerous-write protection: id=%u "
             "offset=0x%08" PRIx32 " size=%" PRIu32,
             component->id, component->target_offset, component->size);
    esp_err_t ret = esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "disable dangerous-write protection");

    ret = esp_flash_erase_region(esp_flash_default_chip, component->target_offset, component->size);
    if (ret == ESP_OK) {
        /* Yield between Recovery writes so the UI can paint real Flash progress. */
        for (uint32_t written = 0; ret == ESP_OK && written < component->size;) {
            uint32_t count = component->size - written;
            if (recovery && count > 64 * 1024) count = 64 * 1024;
            ret = esp_flash_write(esp_flash_default_chip, image + written, component->target_offset + written, count);
            if (ret != ESP_OK) break;
            written += count;
            if (recovery) {
                taskENTER_CRITICAL(&ctx->lock);
                ctx->status.commit_written = written;
                taskEXIT_CRITICAL(&ctx->lock);
                vTaskDelay(1);
            }
        }
    }

    esp_err_t protect_ret = esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    if (protect_ret != ESP_OK) {
        ESP_LOGE(TAG, "restore dangerous-write protection failed: %s",
                 esp_err_to_name(protect_ret));
        if (ret == ESP_OK) {
            ret = protect_ret;
        }
    } else {
        ESP_LOGI(TAG, "dangerous-write protection restored: id=%u", component->id);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "write protected component");

    ESP_RETURN_ON_ERROR(update_hash_flash_region(component->target_offset, component->size, digest),
                        TAG, "read back protected component");
    return update_bytes_equal(digest, component->sha256, 32) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

const factory_update_plan_component_t *
update_plan_for_kind(update_context_t *ctx, esp_iris_system_update_component_kind_t kind)
{
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
        if (ctx->transaction.plan[i].descriptor.kind == kind) {
            return &ctx->transaction.plan[i];
        }
    }
    return NULL;
}

esp_err_t update_commit_update(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                               void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    ESP_RETURN_ON_FALSE(operation_id != NULL && ctx->transaction.prepared &&
                            ctx->transaction.active_index < 0 &&
                            update_bytes_equal(operation_id, ctx->transaction.operation_id,
                                               sizeof(ctx->transaction.operation_id)),
                        ESP_ERR_INVALID_STATE, TAG, "commit state");
    update_status_finish(ctx, ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING, ESP_OK);
    for (size_t i = 0; i < ctx->transaction.plan_count; ++i) {
        ESP_RETURN_ON_FALSE(ctx->transaction.plan[i].completed, ESP_ERR_INVALID_STATE, TAG,
                            "component %u incomplete", ctx->transaction.plan[i].descriptor.id);
    }

    const factory_update_plan_component_t *bootloader =
        update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER);
    const factory_update_plan_component_t *partition_table =
        update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE);
    const factory_update_plan_component_t *application =
        update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION);
    const factory_update_plan_component_t *recovery =
        update_plan_for_kind(ctx, ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY);

    if (application != NULL) {
        ESP_RETURN_ON_FALSE(application->target_partition_valid && application->image_verified &&
                                ctx->transaction.application_received,
                            ESP_ERR_INVALID_STATE, TAG, "application verification is missing");
    }

    if (ctx->transaction.remote_bridge || update_owner_is(ctx, FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE)) {
        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, MOSAICO_RECOVERY_PARTITION);
        ESP_RETURN_ON_FALSE(factory != NULL, ESP_ERR_NOT_FOUND, TAG,
                            "Recovery partition missing before commit");
        ESP_RETURN_ON_ERROR(mosaico_boot_request_recovery(), TAG,
                            "select Recovery before critical commit");
        ctx->transaction.commit_started = true;
    }

    ESP_LOGI(TAG, "system update commit started");

    /* This product deliberately accepts the ESP32-S31 single-copy commit
     * policy: bootloader first, partition table last, with mandatory readback.
     * The five recovery-critical partitions are immutable. Application and
     * data images were already written through descriptors derived from the
     * validated target partition table. */
    if (bootloader != NULL) {
        ESP_LOGI(TAG, "committing bootloader");
    }
    ESP_RETURN_ON_ERROR(
        update_commit_protected_image(ctx, bootloader, ctx->transaction.bootloader_image), TAG,
        "commit bootloader");
    if (bootloader != NULL) {
        uint32_t image_length = 0;
        ESP_RETURN_ON_ERROR(esp_image_verify_bootloader(&image_length), TAG,
                            "verify committed bootloader");
        ESP_LOGI(TAG, "bootloader committed and verified: image_length=%" PRIu32, image_length);
    }
    if (recovery != NULL) {
        ESP_LOGW(TAG, "committing running recovery image from PSRAM");
    }
    ESP_RETURN_ON_ERROR(update_commit_protected_image(ctx, recovery, ctx->transaction.recovery_image),
                        TAG, "commit recovery");
    if (recovery != NULL) {
        esp_partition_pos_t recovery_position = {
            .offset = recovery->descriptor.target_offset,
            .size = recovery->descriptor.size,
        };
        esp_image_metadata_t metadata = {0};
        ESP_RETURN_ON_ERROR(esp_image_verify(ESP_IMAGE_VERIFY, &recovery_position, &metadata), TAG,
                            "verify committed recovery image");
        ESP_LOGI(TAG, "recovery image committed and verified");
    }
    if (partition_table != NULL) {
        ESP_LOGI(TAG, "committing partition table");
        taskENTER_CRITICAL(&ctx->lock);
        ctx->layout_needs_reload = true;
        taskEXIT_CRITICAL(&ctx->lock);
    }
    ESP_RETURN_ON_ERROR(
        update_commit_protected_image(ctx, partition_table, ctx->transaction.partition_table_image), TAG,
        "commit partition table");
    if (partition_table != NULL) {
        ESP_LOGI(TAG, "partition table committed and verified");
    }
    if (ctx->transaction.application_received || recovery != NULL) {
        ESP_RETURN_ON_ERROR(esp_iris_crash_loop_reset(), TAG, "reset Iris crash-loop state");
        ESP_RETURN_ON_ERROR(esp_iris_mark_planned_restart(), TAG,
                            "record planned system-update restart");
    }
    if (ctx->transaction.application_received && !ctx->transaction.remote_bridge) {
        ESP_RETURN_ON_FALSE(application != NULL && application->target_partition_valid &&
                                application->completed && application->image_verified,
                            ESP_ERR_INVALID_STATE, TAG, "application target descriptor is missing");
        const esp_partition_t *ota_partition = &application->target_partition;
        ESP_LOGI(TAG, "selecting ota_0 for next boot: label=%s offset=0x%08" PRIx32,
                 ota_partition->label, ota_partition->address);
        if (ctx->transaction.defer_restart) {
            /* This transaction already verified the NOR readback digest and the
             * complete application image in end_component. Completed components
             * cannot be rewritten. Keep the validated new layout descriptor and
             * avoid repeating the same image verification during boot selection. */
            ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition_skip_validate(ota_partition), TAG,
                                "select Ideas application");
            ESP_RETURN_ON_ERROR(mosaico_boot_consume_request(), TAG, "clear Vibe Mode boot intent");
        } else {
            ESP_RETURN_ON_ERROR(mosaico_boot_select(ota_partition->address), TAG, "select ota_0");
        }
        const esp_partition_t *configured = esp_ota_get_boot_partition();
        ESP_RETURN_ON_FALSE(configured != NULL, ESP_ERR_NOT_FOUND, TAG,
                            "read configured boot partition");
        ESP_LOGI(TAG, "configured boot partition: label=%s offset=0x%08" PRIx32, configured->label,
                 configured->address);
        ESP_RETURN_ON_FALSE(configured->subtype == ota_partition->subtype, ESP_ERR_INVALID_STATE,
                            TAG, "configured boot partition does not match ota_0");
    }
    if (recovery != NULL || ctx->transaction.remote_bridge) {
        const esp_partition_t *factory_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, MOSAICO_RECOVERY_PARTITION);
        ESP_RETURN_ON_FALSE(factory_partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                            "factory partition missing after self-update");
        ESP_RETURN_ON_ERROR(mosaico_boot_request_recovery(), TAG, "select updated recovery");
        bool recovery_requested = false;
        ESP_RETURN_ON_ERROR(mosaico_boot_recovery_requested(&recovery_requested), TAG,
                            "read committed Vibe Mode boot intent");
        ESP_RETURN_ON_FALSE(recovery_requested, ESP_ERR_INVALID_STATE, TAG,
                            "configured boot partition does not match recovery");
    }
    ESP_RETURN_ON_ERROR(update_persist_result(operation_id, ESP_OK), TAG,
                        "persist system update result");
    if (!ctx->transaction.remote_bridge && !ctx->transaction.defer_restart) {
        /* The host may also request a restart after receiving the commit receipt.
         * Share Iris's single deadline so both requests cannot enter esp_restart()
         * concurrently while shutdown handlers are still running. */
        ESP_RETURN_ON_ERROR(esp_iris_schedule_restart(FACTORY_SYSTEM_RESTART_DELAY_MS),
                            TAG, "schedule restart");
    }
    update_status_finish(ctx, ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED, ESP_OK);
    ESP_LOGI(TAG, "system update committed; restart %s",
             ctx->transaction.defer_restart ? "deferred" : "scheduled");
    return ESP_OK;
}

#endif
