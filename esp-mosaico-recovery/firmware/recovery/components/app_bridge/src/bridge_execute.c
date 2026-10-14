// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

esp_err_t bridge_get_table(bridge_context_t *ctx, const cJSON *plan, uint8_t raw[4096])
{
    uint32_t size = bridge_num(plan, "table_size");
    if (!size || size > 4096)
        return ESP_ERR_INVALID_SIZE;
    const char *id = bridge_str(plan, "table_upload_id");
    if (!*id || strchr(id, '/'))
        return ESP_ERR_INVALID_ARG;
    char path[256];
    snprintf(path, sizeof(path), "/api/v1/device-sessions/%s/files/%s", ctx->session, id);
    esp_http_client_handle_t h = bridge_open_request(ctx, path, NULL);
    if (!h)
        return ESP_FAIL;
    memset(raw, 0xff, 4096);
    size_t received = 0;
    esp_err_t err = ESP_FAIL;
    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (received < size) {
        if (bridge_should_cancel(ctx) || esp_timer_get_time() >= deadline)
            goto done;
        int n = esp_http_client_read(h, (char *)raw + received, size - received);
        if (n <= 0)
            goto done;
        received += n;
    }
    char extra;
    if (esp_http_client_read(h, &extra, 1) != 0 || !esp_http_client_is_complete_data_received(h))
        goto done;
    if (bridge_matches(raw, size, bridge_str(plan, "table_sha256")) &&
        bridge_matches(raw, 4096, bridge_str(plan, "target_table_sha256")))
        err = ESP_OK;
done:
    bridge_finish_download(ctx, err == ESP_OK);
    return err;
}

cJSON *bridge_component(unsigned id, const char *kind, uint32_t offset, uint32_t size,
                        const char *sha, const char *file)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", id);
    cJSON_AddStringToObject(j, "kind", kind);
    cJSON_AddNumberToObject(j, "target_offset", offset);
    cJSON_AddNumberToObject(j, "size", size);
    cJSON_AddStringToObject(j, "sha256", sha);
    cJSON_AddStringToObject(j, "file", file);
    return j;
}

esp_err_t bridge_transfer(bridge_context_t *ctx, const char *file,
                          const esp_iris_system_update_component_t *c, const uint8_t *table)
{
    const int64_t started = esp_timer_get_time();
    esp_http_client_handle_t h = NULL;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t hash[32];
    size_t written = 0;
    uint32_t received = 0;
    int64_t read_us = 0, write_us = 0, max_read_us = 0;
    esp_err_t err = ESP_FAIL;
    psa_hash_operation_t digest_op = PSA_HASH_OPERATION_INIT;
    if (!buf)
        return ESP_ERR_NO_MEM;
    if (!table) {
        char path[256];
        snprintf(path, sizeof(path), "/api/v1/device-sessions/%s/files/%s", ctx->session, file);
        h = bridge_open_request(ctx, path, NULL);
        if (!h)
            goto done;
    }
    if (psa_hash_setup(&digest_op, PSA_ALG_SHA_256) != PSA_SUCCESS)
        goto done;
    size_t preloaded = 0;
    if (c->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION ||
        c->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY) {
        if (c->size < 24) {
            err = ESP_ERR_IMAGE_INVALID;
            goto done;
        }
        if (table) {
            memcpy(buf, table, 24);
            preloaded = 24;
        } else
            while (preloaded < 24) {
                int n = esp_http_client_read(h, (char *)buf + preloaded, 24 - preloaded);
                if (n <= 0) {
                    err = ESP_FAIL;
                    goto done;
                }
                preloaded += n;
            }
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        uint32_t flash = 0;
        esp_flash_get_size(NULL, &flash);
        uint16_t min = (uint16_t)buf[15] | ((uint16_t)buf[16] << 8),
                 max = (uint16_t)buf[17] | ((uint16_t)buf[18] << 8);
        unsigned capacity = buf[3] >> 4, speed = buf[3] & 15;
        if (buf[0] != 0xe9 || !buf[1] || buf[1] > 16 || buf[12] != 32 || buf[13] != 0 ||
            chip.revision < min || (max != 0 && max != 65535 && chip.revision > max) ||
            buf[2] > 5 || (speed > 2 && speed != 15) || capacity > 7 ||
            ((uint64_t)1 << 20 << capacity) > flash || buf[23] > 1) {
            err = ESP_ERR_IMAGE_INVALID;
            goto done;
        }
    }
    err = factory_system_update_source_begin_component(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, c);
    if (err != ESP_OK)
        goto done;
    while (received < c->size) {
        if (bridge_should_cancel(ctx)) {
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }
        size_t wanted = c->size - received;
        if (wanted > 4096)
            wanted = 4096;
        int n;
        if (preloaded) {
            n = preloaded;
            preloaded = 0;
        } else if (table) {
            memcpy(buf, table + received, wanted);
            n = wanted;
        } else {
            const int64_t read_started = esp_timer_get_time();
            n = esp_http_client_read(h, (char *)buf, wanted);
            const int64_t elapsed = esp_timer_get_time() - read_started;
            read_us += elapsed;
            if (elapsed > max_read_us)
                max_read_us = elapsed;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (psa_hash_update(&digest_op, buf, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
            goto done;
        }
        const int64_t write_started = esp_timer_get_time();
        err = factory_system_update_source_write_component(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, c,
                                                           received, buf, n);
        write_us += esp_timer_get_time() - write_started;
        if (err != ESP_OK)
            goto done;
        received += n;
    }
    if (h) {
        char extra;
        if (esp_http_client_read(h, &extra, 1) != 0 ||
            !esp_http_client_is_complete_data_received(h)) {
            err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
    }
    if (psa_hash_finish(&digest_op, hash, 32, &written) != PSA_SUCCESS || written != 32) {
        err = ESP_FAIL;
        goto done;
    }
    err = factory_system_update_source_end_component(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, c, hash);
done:
    psa_hash_abort(&digest_op);
    free(buf);
    if (h)
        bridge_finish_download(ctx, err == ESP_OK);
    ESP_LOGI("iris_bridge",
             "component %u: received=%lu/%lu bytes elapsed=%lld ms read=%lld ms write=%lld ms "
             "max_read=%lld ms stack_free=%u result=%s",
             (unsigned)c->id, (unsigned long)received, (unsigned long)c->size,
             (long long)((esp_timer_get_time() - started) / 1000), (long long)(read_us / 1000),
             (long long)(write_us / 1000), (long long)(max_read_us / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL), esp_err_to_name(err));
    return err;
}

esp_err_t bridge_execute(bridge_context_t *ctx, const char *op, const cJSON *plan)
{
    ctx->cancelled = atomic_load(&ctx->stop_requested);
    if (ctx->cancelled)
        return ESP_ERR_INVALID_STATE;
    const char *mode = bridge_str(plan, "mode");
    bool system = strcmp(mode, "system_update") == 0;
    bool layout = strcmp(mode, "layout") == 0;
    bool factory = strcmp(mode, "factory") == 0;
    if ((system && !ctx->cfg.enable_system_update) ||
        (factory && !ctx->cfg.enable_factory_update) ||
        (!system && !layout && !factory && strcmp(mode, "partitions") != 0))
        return ESP_ERR_NOT_SUPPORTED;
    if (system) {
        esp_err_t policy = iris_bridge_validate_system_plan(plan, ctx->cfg.enable_bootloader_update,
                                                            ctx->cfg.enable_factory_update);
        if (policy != ESP_OK)
            return policy;
    }
    if (factory) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(plan, "factory_manifest");
        if (strcmp(bridge_str(m, "profile_id"), "iris-s31-test-layout-v2") ||
            strcmp(bridge_str(m, "board_id"), ctx->cfg.board_id) ||
            bridge_num(m, "protocol_version") != 2 || !*bridge_str(m, "recovery_version"))
            return ESP_ERR_INVALID_VERSION;
    }
    uint8_t *recovery = NULL;
    wifi_ps_type_t saved_ps = WIFI_PS_MIN_MODEM;
    bool restore_ps = false;
    uint8_t opid[16];
    uint8_t *table = NULL;
    if (strlen(op) != 32)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 16; i++) {
        unsigned byte;
        char part[3] = {op[i * 2], op[i * 2 + 1], 0};
        if (strspn(part, "0123456789abcdef") != 2 || sscanf(part, "%2x", &byte) != 1)
            return ESP_ERR_INVALID_ARG;
        opid[i] = byte;
    }

    esp_err_t err = factory_system_update_source_reserve(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, opid);
    if (err != ESP_OK)
        return err;
    char source_hash[65];
    if (!bridge_flash_hash(0x8000, 4096, source_hash) ||
        strcmp(source_hash, bridge_str(plan, "source_table_sha256"))) {
        err = ESP_ERR_INVALID_VERSION;
        goto abort;
    }
    /* Bulk HTTPS should not wait for DTIM wakeups. Restore the caller's mode
     * on every abort and after the final server acknowledgement. */
    if (esp_wifi_get_ps(&saved_ps) == ESP_OK && saved_ps != WIFI_PS_NONE)
        restore_ps = esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    err = bridge_cancel_poll_start(ctx);
    if (err != ESP_OK)
        goto abort;
    if (system) {
        const cJSON *manifest = cJSON_GetObjectItemCaseSensitive(plan, "system_manifest");
        err = factory_system_update_source_prepare_bridge(manifest, opid,
                                                          ctx->cfg.enable_bootloader_update);
        if (err != ESP_OK)
            goto abort;
        goto prepared;
    }
    if (layout) {
        table = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!table) {
            err = ESP_ERR_NO_MEM;
            goto abort;
        }
        err = bridge_get_table(ctx, plan, table);
        if (err != ESP_OK)
            goto abort;
    }
    if (factory) {
        const cJSON *images = cJSON_GetObjectItemCaseSensitive(plan, "images");
        const cJSON *im = cJSON_GetArrayItem(images, 0);
        uint32_t length = bridge_num(im, "size");
        const char *file = bridge_str(im, "upload_id");
        if (cJSON_GetArraySize(images) != 1 || strcmp(bridge_str(im, "partition"), "vibe_mode") ||
            !length || length > 0x1c0000 || !*file || strchr(file, '/')) {
            err = ESP_ERR_INVALID_ARG;
            goto abort;
        }
        recovery = heap_caps_malloc(0x1c0000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!recovery) {
            err = ESP_ERR_NO_MEM;
            goto abort;
        }
        memset(recovery, 0xff, 0x1c0000);
        char path[256];
        snprintf(path, sizeof(path), "/api/v1/device-sessions/%s/files/%s", ctx->session, file);
        esp_http_client_handle_t h = bridge_open_request(ctx, path, NULL);
        if (!h) {
            err = ESP_FAIL;
            goto abort;
        }
        uint32_t done = 0;
        const int64_t deadline = esp_timer_get_time() + 25LL * 60 * 1000000;
        while (done < length) {
            if (bridge_should_cancel(ctx) || esp_timer_get_time() >= deadline)
                break;
            size_t wanted = length - done;
            if (wanted > 4096)
                wanted = 4096;
            int n = esp_http_client_read(h, (char *)recovery + done, wanted);
            if (n <= 0)
                break;
            done += n;
        }
        char extra;
        bool complete = !bridge_should_cancel(ctx) && done == length &&
                        esp_http_client_read(h, &extra, 1) == 0 &&
                        esp_http_client_is_complete_data_received(h);
        bridge_finish_download(ctx, complete);
        if (!complete || !bridge_matches(recovery, length, bridge_str(im, "sha256"))) {
            err = ESP_ERR_INVALID_CRC;
            goto abort;
        }
    }
    cJSON *root = cJSON_CreateObject(), *target = cJSON_AddObjectToObject(root, "target"),
          *items = cJSON_AddArrayToObject(root, "components");
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    cJSON_AddStringToObject(root, "schema", "esp-iris-system-update/0.2");
    cJSON_AddBoolToObject(root, "preserve_layout", !layout);
    cJSON_AddStringToObject(root, "target_layout_sha256", bridge_str(plan, "target_table_sha256"));
    cJSON_AddNumberToObject(target, "chip_id", 32);
    cJSON_AddNumberToObject(target, "flash_size", size);
    unsigned index = 1;
    if (layout)
        cJSON_AddItemToArray(items,
                             bridge_component(index++, "partition_table", 0x8000, 4096,
                                              bridge_str(plan, "target_table_sha256"), "table"));
    const cJSON *im = NULL;
    cJSON_ArrayForEach(im, cJSON_GetObjectItemCaseSensitive(plan, "images"))
    {
        const char *name = bridge_str(im, "partition");
        const esp_partition_t *p =
            esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
        /* For new layouts, read type from validated raw table, never infer from
         * label. */
        unsigned type = p ? p->type : 255;
        if (layout) {
            type = 255;
            for (size_t off = 0; off + 32 <= 0xc00; off += 32) {
                if (table[off] != 0xaa || table[off + 1] != 0x50)
                    continue;
                uint32_t addr;
                memcpy(&addr, table + off + 4, 4);
                if (addr == bridge_num(im, "offset")) {
                    type = table[off + 2];
                    break;
                }
            }
        }
        if (type > 1 || !*bridge_str(im, "upload_id") || strchr(bridge_str(im, "upload_id"), '/')) {
            cJSON_Delete(root);
            err = ESP_ERR_INVALID_ARG;
            goto abort;
        }
        if (factory) {
            uint8_t hash[32];
            char h[65];
            if (!bridge_digest(recovery, 0x1c0000, hash)) {
                cJSON_Delete(root);
                err = ESP_FAIL;
                goto abort;
            }
            bridge_hex(hash, 32, h);
            cJSON_AddItemToArray(items, bridge_component(index++, "recovery", 0x20000, 0x1c0000, h,
                                                         bridge_str(im, "upload_id")));
        } else
            cJSON_AddItemToArray(
                items, bridge_component(index++, type == 0 ? "application" : "data",
                                        bridge_num(im, "offset"), bridge_num(im, "size"),
                                        bridge_str(im, "sha256"), bridge_str(im, "upload_id")));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto abort;
    }
    err = factory_system_update_source_prepare(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, (uint8_t *)json,
                                               strlen(json), opid);
    free(json);
    if (err != ESP_OK)
        goto abort;
prepared:;
    size_t count = factory_system_update_source_component_count(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE);
    bridge_notify_progress(ctx, 0);
    if (bridge_should_cancel(ctx)) {
        err = ESP_ERR_INVALID_STATE;
        goto abort;
    }
    for (size_t i = 0; i < count; i++) {
        esp_iris_system_update_component_t c;
        char file[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES];
        err =
            factory_system_update_source_component(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, i, &c, file);
        if (err != ESP_OK)
            goto abort;
        err = bridge_transfer(ctx, file, &c,
                              !system && c.kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE
                                  ? table
                                  : (factory ? recovery : NULL));
        if (err != ESP_OK)
            goto abort;
        bridge_notify_progress(ctx, (i + 1) * 90 / count);
        if (bridge_should_cancel(ctx)) {
            err = ESP_ERR_INVALID_STATE;
            goto abort;
        }
    }
    free(table);
    table = NULL;
    bridge_cancel_poll_join(ctx);
    bridge_set_state(ctx, "VERIFYING", 95, ESP_OK);
    bridge_close_client(&ctx->download_client);
    /* A successful COMMITTING acknowledgement authorizes the critical write.
     * A local stop arriving after that acknowledgement cannot revoke it. */
    if (!bridge_authorize_commit(ctx)) {
        err = ESP_ERR_INVALID_STATE;
        goto abort;
    }
    err = factory_system_update_source_commit(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, opid);
    if (err != ESP_OK) {
        /* A failed readback or metadata write may follow a physical table write.
         * Discard all cached descriptors after an attempted critical commit. */
        if (factory_system_update_source_needs_restart(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE)) {
            bridge_report_result_bounded(ctx, err);
            free(recovery);
            if (restore_ps)
                (void)esp_wifi_set_ps(saved_ps);
            esp_restart();
        }
        goto abort;
    }
    free(recovery);
    recovery = NULL;
    err = bridge_save_boot(plan);
    if (err != ESP_OK) {
        esp_err_t stored = factory_system_metadata_store_last_result(opid, err);
        ESP_LOGW("iris_bridge", "boot intent failed: %s; result persistence=%s",
                 esp_err_to_name(err), esp_err_to_name(stored));
    }
    bridge_report_result_bounded(ctx, err);
    if (restore_ps)
        (void)esp_wifi_set_ps(saved_ps);
    /* The raw layout may have changed: never resume with a stale IDF cache. */
    esp_restart();
    return ESP_OK;
abort:
    free(table);
    bridge_cancel_poll_join(ctx);
    bridge_close_client(&ctx->download_client);
    if (restore_ps)
        (void)esp_wifi_set_ps(saved_ps);
    free(recovery);
    factory_system_update_source_abort(FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE, opid, err);
    return err;
}
