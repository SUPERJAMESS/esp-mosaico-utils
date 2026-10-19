// SPDX-License-Identifier: Apache-2.0
#include "factory_http_client.h"
#include "esp_iris.h"

esp_err_t factory_http_client_set_common_headers(esp_http_client_handle_t client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    char device_id[33];
    if (esp_iris_format_device_id(device_id) == ESP_OK)
        return esp_http_client_set_header(client, FACTORY_HTTP_CLIENT_DEVICE_ID_HEADER, device_id);

    /* Optional metadata must not prevent requests before Iris is available. */
    esp_err_t err = esp_http_client_delete_header(client, FACTORY_HTTP_CLIENT_DEVICE_ID_HEADER);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}
