// SPDX-License-Identifier: Apache-2.0
#include "app_iris_storage.h"
#include "app_storage.h"
#include "factory_nand_update.h"
#include "sdkconfig.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND &&                  \
    CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE

#include <string.h>
#include "esp_check.h"
#include "esp_iris.h"
static const char *TAG = "app_iris_storage";
#define NAND_UPDATE_SERVICE_ID 0x1201U
#define NAND_UPDATE_START_METHOD_ID 2U
static esp_err_t nand_update_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                                 size_t response_capacity, size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(request != NULL && response_size != NULL && request->payload != NULL &&
                            request->payload_size > 0 &&
                            request->payload_size < FACTORY_SYSTEM_UPDATE_PATH_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid NAND update RPC");
    char path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
    memcpy(path, request->payload, request->payload_size);
    path[request->payload_size] = '\0';
    ESP_RETURN_ON_FALSE(memchr(path, '\0', request->payload_size) == NULL, ESP_ERR_INVALID_ARG, TAG,
                        "NAND update path contains NUL");
    ESP_RETURN_ON_ERROR(factory_system_update_start_nand(path), TAG, "start NAND update");
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t nand_status(void *ctx)
{
    (void)ctx;
    return app_storage_access_status();
}

esp_err_t app_iris_storage_register(void)
{
    /* Advertise the descriptor now; the Ideas worker mounts NAND after USB
     * and UI startup. New file requests return BUSY until mounting completes
     * and while a local job owns NAND, rather than waiting in LittleFS. */
    const esp_iris_file_volume_config_t volume = {
        .id = APP_STORAGE_VOLUME_ID,
        .base_path = APP_STORAGE_MOUNT_PATH,
        .capabilities = ESP_IRIS_FILE_VOLUME_READ | ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME | ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE | ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME | ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    };
    esp_err_t file_err = esp_iris_file_volume_register_deferred(&volume, nand_status, NULL);
    if (file_err == ESP_OK) {
        ESP_LOGI(TAG, "registered writable ESP-Iris volume id=%s base=%s", APP_STORAGE_VOLUME_ID,
                 APP_STORAGE_MOUNT_PATH);
    } else {
        ESP_LOGE(TAG, "NAND file volume unavailable: %s", esp_err_to_name(file_err));
    }

    return esp_iris_rpc_register(NAND_UPDATE_SERVICE_ID, NAND_UPDATE_START_METHOD_ID,
                                 nand_update_rpc, NULL);
}

#else
esp_err_t app_iris_storage_register(void)
{
    return ESP_OK;
}
#endif
