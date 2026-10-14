// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

void bridge_set_state(bridge_context_t *ctx, const char *state, unsigned progress, esp_err_t error)
{
    taskENTER_CRITICAL(&ctx->snapshot_lock);
    strlcpy(ctx->snapshot.state, state, sizeof(ctx->snapshot.state));
    ctx->snapshot.progress = progress;
    ctx->snapshot.error = error;
    if (strcmp(state, "PAIRING") != 0) {
        memset(ctx->snapshot.code, 0, sizeof(ctx->snapshot.code));
        ctx->snapshot.expires_at[0] = 0;
        ctx->code_deadline = 0;
    }
    taskEXIT_CRITICAL(&ctx->snapshot_lock);
}

cJSON *bridge_identity(bridge_context_t *ctx)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "chip", "esp32s31");
    cJSON_AddNumberToObject(j, "chip_id", 32);
    cJSON_AddStringToObject(j, "profile_id", "iris-s31-test-layout-v2");
    cJSON_AddStringToObject(j, "mac", ctx->mac);
    cJSON_AddStringToObject(j, "device_id", ctx->cfg.device_id);
    cJSON_AddStringToObject(j, "boot_id", ctx->boot_id);
    return j;
}

bool bridge_inventory(bridge_context_t *ctx)
{
    uint8_t raw[4096];
    unsigned char encoded[5500];
    size_t n;
    char hash[65], path[256];
    uint32_t flash = 0, physical = 0;
    if (esp_flash_read(NULL, raw, 0x8000, sizeof(raw)) != ESP_OK ||
        esp_flash_get_size(NULL, &flash) != ESP_OK ||
        esp_flash_get_physical_size(NULL, &physical) != ESP_OK ||
        mbedtls_base64_encode(encoded, sizeof(encoded), &n, raw, sizeof(raw)) != 0)
        return false;
    encoded[n] = 0;
    cJSON *j = bridge_identity(ctx);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddNumberToObject(j, "revision", chip.revision);
    cJSON_AddStringToObject(j, "board_id", ctx->cfg.board_id);
    cJSON_AddStringToObject(j, "recovery_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "idf_version", esp_get_idf_version());
    cJSON_AddNumberToObject(j, "flash_size", flash);
    cJSON_AddNumberToObject(j, "physical_flash_size", physical);
    cJSON_AddNumberToObject(j, "bootloader_offset", 0x2000);
    cJSON_AddNumberToObject(j, "partition_table_offset", 0x8000);
    if (!bridge_flash_hash(0x2000, 0x6000, hash))
        goto fail;
    cJSON_AddStringToObject(j, "bootloader_sha256", hash);
    if (!bridge_flash_hash(0x8000, 0x1000, hash))
        goto fail;
    cJSON_AddStringToObject(j, "partition_table_sha256", hash);
    cJSON_AddStringToObject(j, "partition_table_bin", (char *)encoded);
    cJSON_AddStringToObject(j, "running_partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(j, "secure_boot", esp_secure_boot_enabled());
    cJSON_AddBoolToObject(j, "flash_encryption", esp_efuse_is_flash_encryption_enabled());
    cJSON *caps = cJSON_AddArrayToObject(j, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("write_partitions"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("replace_layout"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("select_boot"));
    if (ctx->cfg.enable_factory_update)
        cJSON_AddItemToArray(caps, cJSON_CreateString("factory_update"));
    if (ctx->cfg.enable_system_update)
        cJSON_AddItemToArray(caps, cJSON_CreateString("system_update"));
    if (ctx->cfg.enable_system_update && ctx->cfg.enable_bootloader_update)
        cJSON_AddItemToArray(caps, cJSON_CreateString("bootloader_update"));
    factory_sysmeta_record_t previous;
    if (factory_system_metadata_load_last_result(&previous) == ESP_OK) {
        char ophex[33];
        bridge_hex(previous.operation_id, 16, ophex);
        cJSON_AddStringToObject(j, "last_operation_id", ophex);
        cJSON_AddStringToObject(j, "last_operation_state",
                                previous.result == ESP_OK ? "DONE" : "INTERRUPTED");
    }
    bridge_path_session(ctx, path, "/inventory");
    cJSON *r = bridge_request(ctx, path, j);
    bool ok = r != NULL;
    cJSON_Delete(r);
    cJSON_Delete(j);
    return ok;
fail:
    cJSON_Delete(j);
    return false;
}

void bridge_run(void *unused)
{
    bridge_context_t *ctx = unused;

    unsigned backoff = 5;
    unsigned registration_failures = 0;
    int64_t pairing_deadline = 0;
    bridge_set_state(ctx, "WAITING_NETWORK", 0, ESP_OK);
    while (!atomic_load(&ctx->stop_requested) && ctx->cfg.network_ready &&
           !ctx->cfg.network_ready()) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!atomic_load(&ctx->stop_requested))
        bridge_set_state(ctx, "REGISTERING", 0, ESP_OK);
    while (!atomic_load(&ctx->stop_requested)) {
        if (pairing_deadline && esp_timer_get_time() >= pairing_deadline) {
            bridge_set_state(ctx, "EXPIRED", 0, ESP_ERR_TIMEOUT);
            if (!atomic_load(&ctx->bridge_active))
                break;
            /* A visible download page renews an expired, unpaired session.
             * Idle prefetch expires instead of registering forever. */
            memset(ctx->session, 0, sizeof(ctx->session));
            memset(ctx->token, 0, sizeof(ctx->token));
            pairing_deadline = 0;
            registration_failures = 0;
            bridge_set_state(ctx, "REGISTERING", 0, ESP_OK);
        }
        if (ctx->session[0] && !atomic_load(&ctx->bridge_active)) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!ctx->session[0]) {
            const int64_t registered_at = esp_timer_get_time();
            cJSON *j = bridge_identity(ctx);
            cJSON *r = bridge_request(ctx, "/api/v1/device-sessions", j);
            cJSON_Delete(j);
            if (r) {
                const char *id = bridge_str(r, "session_id");
                const char *secret = bridge_str(r, "auth_token");
                const char *code = bridge_str(r, "device_code");
                if (bridge_num(r, "control_protocol") != 2 || strlen(id) != 32 || !*secret ||
                    strlen(secret) >= sizeof(ctx->token) || !*code ||
                    strlen(code) >= IRIS_BRIDGE_CODE_BYTES) {
                    cJSON_Delete(r);
                    bridge_set_state(ctx, "FAILED", 0, ESP_ERR_INVALID_RESPONSE);
                    break;
                }
                strlcpy(ctx->session, id, sizeof(ctx->session));
                strlcpy(ctx->token, secret, sizeof(ctx->token));
                ESP_LOGI("iris_bridge", "control protocol 2");
                pairing_deadline = registered_at + 600LL * 1000000;
                bridge_set_state(ctx, "PAIRING", 0, ESP_OK);
                taskENTER_CRITICAL(&ctx->snapshot_lock);
                strlcpy(ctx->snapshot.code, code, sizeof(ctx->snapshot.code));
                strlcpy(ctx->snapshot.expires_at, bridge_str(r, "expires_at"),
                        sizeof(ctx->snapshot.expires_at));
                ctx->code_deadline = pairing_deadline;
                taskEXIT_CRITICAL(&ctx->snapshot_lock);
                cJSON_Delete(r);
                backoff = 5;
            } else {
                if (++registration_failures >= 3) {
                    bridge_set_state(ctx, "FAILED", 0, ESP_FAIL);
                    break;
                }
                backoff = backoff < 30 ? backoff * 2 : 60;
            }
        } else {
            char path[256];
            bridge_path_session(ctx, path, "/poll");
            cJSON *r = bridge_request(ctx, path, NULL);
            /* A page may close during a blocking HTTP request. Never promote
             * a background session into inventory/update work on its reply. */
            if (!atomic_load(&ctx->bridge_active)) {
                cJSON_Delete(r);
                continue;
            }
            if (r) {
                backoff = 5;
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "inventory_required"))) {
                    pairing_deadline = 0;
                    bridge_set_state(ctx, "PAIRED", 0, ESP_OK);
                    bridge_inventory(ctx);
                }
                const cJSON *flash = cJSON_GetObjectItemCaseSensitive(r, "flash");
                if (!atomic_load(&ctx->stop_requested) && atomic_load(&ctx->bridge_active) &&
                    !strcmp(bridge_str(flash, "phase"), "QUEUED")) {
                    /* Never replay after an uncertain PRECHECK acknowledgement. */
                    bridge_path_session(ctx, path, "/authorize");
                    cJSON *start = cJSON_CreateObject();
                    cJSON_AddStringToObject(start, "phase", "PRECHECK");
                    cJSON_AddNumberToObject(start, "progress", 0);
                    bridge_set_state(ctx, "PRECHECK", 0, ESP_OK);
                    const int64_t requested_at = esp_timer_get_time();
                    cJSON *accepted = bridge_request(ctx, path, start);
                    cJSON_Delete(start);
                    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
                    uint32_t ttl = bridge_num(accepted, "write_authorization_ttl_ms");
                    ctx->write_deadline = requested_at + (int64_t)ttl * 1000;
                    if (ttl && !bridge_should_cancel(ctx) && accepted &&
                        atomic_load(&ctx->bridge_active) &&
                        !strcmp(bridge_str(accepted, "phase"), "PRECHECK"))
                        err = bridge_execute(ctx, ctx->session, accepted);
                    if (err != ESP_OK)
                        bridge_terminal_event(ctx,
                                              ctx->cancelled || atomic_load(&ctx->stop_requested) ||
                                                      !atomic_load(&ctx->bridge_active)
                                                  ? "CANCELLED"
                                                  : "FAILED",
                                              0, err);
                    cJSON_Delete(accepted);
                    cJSON_Delete(r);
                    break;
                }
                cJSON_Delete(r);
            } else if (ctx->main_response.status == 401 || ctx->main_response.status == 404 ||
                       ctx->main_response.status == 410) {
                bridge_set_state(ctx, "ENDED", 0, ESP_ERR_INVALID_STATE);
                break;
            } else {
                backoff = backoff < 30 ? backoff * 2 : 60;
            }
        }
        if (ctx->session[0] && !atomic_load(&ctx->bridge_active))
            continue;
        unsigned wait_seconds = ctx->main_response.retry_after_seconds > backoff
                                    ? ctx->main_response.retry_after_seconds
                                    : backoff;
        unsigned remaining_ms = wait_seconds * 1000 + esp_random() % 1000;
        while (remaining_ms && !atomic_load(&ctx->stop_requested)) {
            if (ctx->session[0] && !atomic_load(&ctx->bridge_active))
                break;
            unsigned n = remaining_ms > 250 ? 250 : remaining_ms;
            vTaskDelay(pdMS_TO_TICKS(n));
            remaining_ms -= n;
        }
    }
    if (atomic_load(&ctx->stop_requested)) {
        if (ctx->session[0])
            bridge_terminal_event(ctx, "CANCELLED", 0, ESP_ERR_INVALID_STATE);
        else
            bridge_set_state(ctx, "CANCELLED", 0, ESP_OK);
    }
    bridge_close_client(&ctx->main_client);
    bridge_close_client(&ctx->download_client);
    memset(ctx->session, 0, sizeof(ctx->session));
    memset(ctx->token, 0, sizeof(ctx->token));
    atomic_store(&ctx->bridge_running, false);
    vTaskDelete(NULL);
}
