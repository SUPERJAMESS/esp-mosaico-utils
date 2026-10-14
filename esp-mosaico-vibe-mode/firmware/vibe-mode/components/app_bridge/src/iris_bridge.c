// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

static bridge_context_t s_bridge = {
    .snapshot_lock = portMUX_INITIALIZER_UNLOCKED,
    .snapshot = {.state = "IDLE"},
};

void iris_bridge_get_snapshot(iris_bridge_snapshot_t *out)
{
    bridge_context_t *ctx = &s_bridge;
    if (out == NULL) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&ctx->snapshot_lock);
    *out = ctx->snapshot;
    out->running = atomic_load(&ctx->bridge_running);
    out->expires_in_ms = ctx->code_deadline > now ? (ctx->code_deadline - now) / 1000 : 0;
    taskEXIT_CRITICAL(&ctx->snapshot_lock);
    if (out->code[0] && out->expires_in_ms == 0) {
        memset(out->code, 0, sizeof(out->code));
        strlcpy(out->state, "EXPIRED", sizeof(out->state));
    }
}

void iris_bridge_stop(void)
{
    bridge_context_t *ctx = &s_bridge;
    atomic_store(&ctx->stop_requested, true);
}

void iris_bridge_set_active(bool active)
{
    bridge_context_t *ctx = &s_bridge;
    atomic_store(&ctx->bridge_active, active);
}

bool iris_bridge_is_running(void)
{
    bridge_context_t *ctx = &s_bridge;
    return atomic_load(&ctx->bridge_running);
}

esp_err_t iris_bridge_start(const iris_bridge_config_t *config)
{
    bridge_context_t *ctx = &s_bridge;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&ctx->bridge_running, &expected, true))
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if !CONFIG_ESP_IRIS_SYSTEM_UPDATE || !CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND ||                \
    !CONFIG_SPIRAM_XIP_FROM_PSRAM
    goto fail;
#endif
    err = ESP_ERR_INVALID_ARG;
    if (!config || !config->server_url || !config->board_id || !config->device_id)
        goto fail;
    if (!*config->server_url || !*config->board_id) {
        bridge_set_state(ctx, "NOT_CONFIGURED", 0, ESP_ERR_INVALID_STATE);
        atomic_store(&ctx->bridge_running, false);
        return ESP_ERR_INVALID_STATE;
    }
    if (strncmp(config->server_url, "https://", 8) || !config->server_url[8] ||
        strpbrk(config->server_url + 8, "/?#@ \t\r\n") ||
        strlen(config->server_url) >= sizeof(ctx->server_url) ||
        strlen(config->board_id) >= sizeof(ctx->board_id) || strlen(config->device_id) != 32)
        goto fail;
    err = ESP_ERR_NOT_SUPPORTED;
    if (!CONFIG_IDF_TARGET_ESP32S31 || CONFIG_PARTITION_TABLE_OFFSET != 0x8000 ||
        CONFIG_BOOTLOADER_OFFSET_IN_FLASH != 0x2000 || esp_secure_boot_enabled() ||
        esp_efuse_is_flash_encryption_enabled())
        goto fail;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_TEST)
        goto fail;
    uint8_t bytes[16], address[6];
    if (!ctx->boot_id[0]) {
        esp_fill_random(bytes, sizeof(bytes));
        bridge_hex(bytes, sizeof(bytes), ctx->boot_id);
    }
    err = ESP_FAIL;
    if (esp_read_mac(address, ESP_MAC_EFUSE_FACTORY) != ESP_OK || psa_crypto_init() != PSA_SUCCESS)
        goto fail;
    snprintf(ctx->mac, sizeof(ctx->mac), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
             address[2], address[3], address[4], address[5]);
    strlcpy(ctx->server_url, config->server_url, sizeof(ctx->server_url));
    strlcpy(ctx->board_id, config->board_id, sizeof(ctx->board_id));
    strlcpy(ctx->device_id, config->device_id, sizeof(ctx->device_id));
    ctx->cfg = *config;
    ctx->cfg.server_url = ctx->server_url;
    ctx->cfg.board_id = ctx->board_id;
    ctx->cfg.device_id = ctx->device_id;
    taskENTER_CRITICAL(&ctx->snapshot_lock);
    memset(&ctx->snapshot, 0, sizeof(ctx->snapshot));
    strlcpy(ctx->snapshot.server_url, ctx->server_url, sizeof(ctx->snapshot.server_url));
    strlcpy(ctx->snapshot.state, "WAITING_NETWORK", sizeof(ctx->snapshot.state));
    taskEXIT_CRITICAL(&ctx->snapshot_lock);
    ctx->cancelled = false;
    ctx->write_deadline = 0;
    atomic_store(&ctx->bridge_active, !config->prefetch_only);
    atomic_store(&ctx->stop_requested, false);
    /* This task also writes Flash/NVS, so keep its stack internal. The cloud
     * update path uses about 15.2 KiB; leave headroom for TLS/error paths. */
    if (xTaskCreate(bridge_run, "iris_bridge", 20480, ctx, 4, NULL) == pdPASS)
        return ESP_OK;
    err = ESP_ERR_NO_MEM;
fail:
    bridge_set_state(ctx, "FAILED", 0, err);
    atomic_store(&ctx->bridge_running, false);
    return err;
}
