// SPDX-License-Identifier: Apache-2.0
#include "factory_update_check.h"
#include "factory_http_client.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

#define MANIFEST_MAX_BYTES 4096U
#define CHECK_INTERVAL_US (60LL * 1000000)

static const char *TAG = "vibe_update_check";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static factory_update_check_snapshot_t s_snapshot;
static bool s_running;
static int64_t s_last_attempt;

typedef struct {
    char body[MANIFEST_MAX_BYTES + 1];
    size_t length;
    esp_err_t error;
} response_t;

static esp_err_t receive(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if (response->error != ESP_OK) return response->error;
    if ((size_t)event->data_len > MANIFEST_MAX_BYTES - response->length) {
        response->error = ESP_ERR_INVALID_SIZE;
        return response->error;
    }
    memcpy(response->body + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

/* Accept numeric releases and the build suffix used by development images. */
static bool parse_version(const char *text, uint32_t parts[3])
{
    if (!text) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') return false;
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9') {
            unsigned digit = (unsigned)(*text++ - '0');
            if (value > (UINT32_MAX - digit) / 10U) return false;
            value = value * 10U + digit;
        }
        parts[i] = value;
        if (i < 2 && *text++ != '.') return false;
    }
    return !*text || ((*text == '-' || *text == '+') && text[1]);
}

static esp_err_t parse_manifest(const response_t *response, factory_update_check_snapshot_t *result)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(response->body, response->length + 1, &end, true);
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const char *product = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "product"));
    const char *latest = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "latest_version"));
    uint32_t current[3], candidate[3];
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsObject(root) || end != response->body + response->length ||
        !cJSON_IsNumber(schema) || schema->valuedouble != 1 || !product ||
        strcmp(product, CONFIG_IRIS_FACTORY_UPDATE_PRODUCT) || !latest || strlen(latest) >= sizeof(result->version) ||
        !parse_version(esp_app_get_description()->version, current) ||
        !parse_version(latest, candidate)) goto done;

    /* Discovery reports newer releases, including those requiring ROM migration. */
    for (size_t i = 0; i < 3; ++i) {
        if (current[i] == candidate[i]) continue;
        result->available = candidate[i] > current[i];
        break;
    }
    const cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (firmware) {
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(firmware, "file"));
        const char *hash = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(firmware, "sha256"));
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(firmware, "size");
        if (!cJSON_IsObject(firmware) || !file || !file[0] || file[0] == '.' || strlen(file) >= sizeof(result->file) ||
            strspn(file, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != strlen(file) ||
            strlen(file) < 5 || strcmp(file + strlen(file) - 4, ".bin") ||
            !hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 ||
            !cJSON_IsNumber(size) || size->valuedouble < 288 || size->valuedouble > 0x1c0000 ||
            size->valuedouble != (uint32_t)size->valuedouble) goto done;
        strlcpy(result->file, file, sizeof(result->file));
        strlcpy(result->sha256, hash, sizeof(result->sha256));
        result->size = (uint32_t)size->valuedouble;
        result->installable = result->available && candidate[0] == current[0];
    }
    strlcpy(result->version, latest, sizeof(result->version));
    err = ESP_OK;
done:
    cJSON_Delete(root);
    return err;
}

static void check_complete(const factory_update_check_snapshot_t *result, esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t generation = s_snapshot.generation + 1;
    /* Keep a known release on transient failures, but expose this check result. */
    if (result) s_snapshot = *result;
    s_snapshot.generation = generation;
    s_snapshot.error = error;
    s_running = false;
    portEXIT_CRITICAL(&s_lock);
}

static void check_task(void *arg)
{
    (void)arg;
    factory_update_check_snapshot_t result = {0};
    response_t *response = calloc(1, sizeof(*response));
    esp_err_t err = ESP_ERR_NO_MEM;
    int status = 0;
    if (response) {
        const esp_http_client_config_t config = {
            .url = CONFIG_IRIS_FACTORY_UPDATE_MANIFEST_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = receive,
            .user_data = response,
            .user_agent = "ESP-Mosaico-Vibe-Mode",
            .timeout_ms = 10000,
            .buffer_size = 1024,
            .buffer_size_tx = 512,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_header(client, "Accept", "application/json");
            esp_http_client_set_header(client, "Cache-Control", "no-cache");
            err = factory_http_client_set_common_headers(client);
            if (err == ESP_OK) err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            if (err == ESP_OK) err = response->error;
            if (err == ESP_OK && (status != 200 || !esp_http_client_is_complete_data_received(client))) err = ESP_ERR_INVALID_RESPONSE;
            if (err == ESP_OK) err = parse_manifest(response, &result);
            esp_http_client_cleanup(client);
        }
        free(response);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Update check failed: %s (HTTP %d)", esp_err_to_name(err), status);
    else ESP_LOGI(TAG, "Published version %s, update available=%d", result.version, result.available);
    check_complete(err == ESP_OK ? &result : NULL, err);
    vTaskDelete(NULL);
}

void factory_update_check_request(bool manual)
{
    const char *url = CONFIG_IRIS_FACTORY_UPDATE_MANIFEST_URL;
    if (!url[0]) {
        check_complete(NULL, ESP_ERR_NOT_SUPPORTED);
        return;
    }
    if (strncmp(url, "https://", 8)) {
        check_complete(NULL, ESP_ERR_INVALID_ARG);
        ESP_LOGE(TAG, "Update manifest URL must use HTTPS");
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_running || (!manual && s_last_attempt && now - s_last_attempt < CHECK_INTERVAL_US)) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_running = true;
    s_last_attempt = now;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(check_task, "vibe_check", 6144, NULL, 3, NULL) != pdPASS) {
        check_complete(NULL, ESP_ERR_NO_MEM);
        ESP_LOGE(TAG, "Could not start update check");
    }
}

void factory_update_check_snapshot(factory_update_check_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    out->checking = s_running;
    portEXIT_CRITICAL(&s_lock);
}
