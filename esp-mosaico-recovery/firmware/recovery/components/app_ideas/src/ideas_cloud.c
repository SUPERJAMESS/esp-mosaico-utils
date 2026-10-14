// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

esp_err_t ideas_http_open(const char *path, const char *body, esp_http_client_handle_t *out)
{
    const char *origin = CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL;
    const char *host = origin + (strlen(origin) >= 8 ? 8 : 0);
    if (strncmp(origin, "https://", 8) || !*host || strpbrk(host, "/?#@\r\n") ||
        !CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID[0])
        return ESP_ERR_INVALID_STATE;
    char url[512];
    int n = snprintf(url, sizeof(url), "%s%s", origin, path);
    if (n < 0 || n >= sizeof(url))
        return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t config = {.url = url,
                                       .timeout_ms = 15000,
                                       .disable_auto_redirect = true,
                                       .crt_bundle_attach = esp_crt_bundle_attach,
                                       .buffer_size = IO_BYTES,
                                       .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_ERR_NO_MEM;
    if (body)
        esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, body ? strlen(body) : 0);
    if (err == ESP_OK && body) {
        size_t sent = 0, len = strlen(body);
        while (sent < len) {
            int written = esp_http_client_write(client, body + sent, len - sent);
            if (written <= 0) {
                err = ESP_FAIL;
                break;
            }
            sent += written;
        }
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0)
        err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200)
        err = esp_http_client_get_status_code(client) == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }
    *out = client;
    return ESP_OK;
}

esp_err_t ideas_resolve(ideas_context_t *ctx, const job_t *job, const ideas_package_t *local,
                        char **json, size_t *size)
{
    factory_network_snapshot_t network;
    if (factory_network_get_snapshot(&network) != ESP_OK ||
        network.state != FACTORY_NETWORK_CONNECTED)
        return ESP_ERR_INVALID_STATE;
    cJSON *body = cJSON_CreateObject();
    if (!body)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "code", job->code);
    cJSON_AddStringToObject(body, "board_id", CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID);
    cJSON_AddStringToObject(body, "locale", CONFIG_IRIS_FACTORY_IDEAS_LOCALE);
    cJSON_AddNumberToObject(body, "recovery_abi", 2);
    cJSON_AddNumberToObject(body, "layout_version", 5);
    cJSON_AddStringToObject(body, "download_schema", "mosaico-ideas/download/1");
    cJSON_AddStringToObject(body, "display_schema", "mosaico-ideas/display/1");
    cJSON_AddStringToObject(body, "font_format", "MIFF/1");
    uint32_t flash_size = 0;
    esp_flash_get_size(esp_flash_default_chip, &flash_size);
    cJSON_AddNumberToObject(body, "flash_size", flash_size);
    if (local) {
        cJSON_AddStringToObject(body, "application_id", local->application_id);
        cJSON_AddStringToObject(body, "current_release_id", local->release_id);
    }
    char *encoded = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!encoded)
        return ESP_ERR_NO_MEM;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ideas_http_open("/api/v1/ideas/resolve", encoded, &client);
    cJSON_free(encoded);
    if (err != ESP_OK)
        return err;
    char *buffer = malloc(IDEAS_DESCRIPTOR_BYTES + 1);
    size_t used = 0;
    if (!buffer)
        err = ESP_ERR_NO_MEM;
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (err == ESP_OK && used <= IDEAS_DESCRIPTOR_BYTES) {
        if (ideas_cancelled(ctx, job->request_id)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        int n = esp_http_client_read(client, buffer + used, IDEAS_DESCRIPTOR_BYTES + 1 - used);
        if (n < 0 || esp_timer_get_time() > deadline) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!n)
            break;
        used += n;
        if (used > IDEAS_DESCRIPTOR_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_INVALID_SIZE;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }
    buffer[used] = 0;
    *json = buffer;
    *size = used;
    return ESP_OK;
}

esp_err_t ideas_fetch_file(const ideas_file_t *file, const char *path, void *argument)
{
    ideas_context_t *ctx = ((pending_t *)argument)->owner;
    const pending_t *pending = argument;
    const icon_t *asset = NULL;
    for (size_t i = 0; pending && i < pending->package.file_count; ++i)
        if (!strcmp(file->name, pending->package.files[i].name))
            asset = &pending->assets[i];
    if (asset && asset->data) {
        FILE *cached = fopen(path, "wb");
        if (!cached)
            return ESP_FAIL;
        bool ok = fwrite(asset->data, 1, asset->size, cached) == asset->size;
        if (fflush(cached) || fsync(fileno(cached)))
            ok = false;
        if (fclose(cached))
            ok = false;
        if (ok) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            ctx->state->received += asset->size;
            xSemaphoreGive(ctx->lock);
        }
        return ok ? ESP_OK : ESP_FAIL;
    }
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ideas_http_open(file->path, NULL, &client);
    if (err != ESP_OK)
        return err;
    FILE *f = fopen(path, "wb");
    uint8_t *buffer = malloc(IO_BYTES);
    int64_t length = esp_http_client_get_content_length(client);
    if (!f || !buffer)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && length >= 0 && length != file->size)
        err = ESP_ERR_INVALID_SIZE;
    uint32_t used = 0;
    int64_t deadline = esp_timer_get_time() + 300000000;
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, (char *)buffer, IO_BYTES);
        if (n < 0 || esp_timer_get_time() > deadline) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!n)
            break;
        if ((uint64_t)used + n > file->size) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buffer, 1, n, f) != n) {
            err = ESP_FAIL;
            break;
        }
        used += n;
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        ctx->state->received += n;
        xSemaphoreGive(ctx->lock);
        vTaskDelay(1);
    }
    if (err == ESP_OK && (used != file->size || !esp_http_client_is_complete_data_received(client)))
        err = ESP_ERR_INVALID_SIZE;
    if (f) {
        if (fflush(f) || fsync(fileno(f)))
            err = ESP_FAIL;
        if (fclose(f))
            err = ESP_FAIL;
    }
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ideas_fetch_asset(ideas_context_t *ctx, const job_t *job, pending_t *pending,
                            unsigned index, bool local)
{
    if (ideas_cancelled(ctx, job->request_id))
        return ESP_ERR_INVALID_STATE;
    const ideas_file_t *file = &pending->package.files[index];
    icon_t *asset = &pending->assets[index];
    asset->data = heap_caps_malloc(file->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!asset->data)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    size_t used = 0;
    if (local) {
        char path[IDEAS_PATH_BYTES];
        err = ideas_store_path(ROOT, pending->package.code, file->name, path);
        FILE *f = err == ESP_OK ? fopen(path, "rb") : NULL;
        if (!f)
            return ESP_ERR_NOT_FOUND;
        used = fread(asset->data, 1, file->size, f);
        if (used != file->size || ferror(f) || fgetc(f) != EOF)
            err = ESP_ERR_INVALID_SIZE;
        fclose(f);
    } else {
        esp_http_client_handle_t client = NULL;
        err = ideas_http_open(file->path, NULL, &client);
        if (err != ESP_OK)
            return err;
        int64_t length = esp_http_client_get_content_length(client);
        if (length >= 0 && length != file->size)
            err = ESP_ERR_INVALID_SIZE;
        int64_t deadline = esp_timer_get_time() + 30000000;
        while (err == ESP_OK && used < file->size) {
            if (ideas_cancelled(ctx, job->request_id)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            size_t nmax = file->size - used;
            if (nmax > IO_BYTES)
                nmax = IO_BYTES;
            int n = esp_http_client_read(client, (char *)asset->data + used, nmax);
            if (n < 0 || esp_timer_get_time() > deadline) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if (!n)
                break;
            used += n;
            vTaskDelay(1);
        }
        if (err == ESP_OK &&
            (used != file->size || !esp_http_client_is_complete_data_received(client)))
            err = ESP_ERR_INVALID_SIZE;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    uint8_t digest[32];
    size_t size = 0;
    char actual[65];
    if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS ||
                          psa_hash_compute(PSA_ALG_SHA_256, asset->data, used, digest,
                                           sizeof(digest), &size) != PSA_SUCCESS ||
                          size != 32))
        err = ESP_FAIL;
    if (err == ESP_OK) {
        ideas_hex(digest, actual);
        if (strcmp(actual, file->sha256))
            err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK)
        asset->size = used;
    return err;
}
