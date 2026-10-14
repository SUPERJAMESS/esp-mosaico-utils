#pragma once
#include "backend_sdk.h"
#define ESP_ERR_NOT_ALLOWED 13
#define ESP_IRIS_TRANSPORT_KIND_USB 1
#define ESP_IRIS_TRANSPORT_KIND_TCP 2
typedef struct {
    bool session_ready;
    int transport;
} esp_iris_status_t;
typedef struct {
    const uint8_t *payload;
    size_t payload_size;
} esp_iris_rpc_request_t;
typedef esp_err_t (*mock_rpc_handler)(const esp_iris_rpc_request_t *, uint8_t *, size_t,
                                      size_t *, void *);
esp_err_t esp_iris_get_status(esp_iris_status_t *status);
esp_err_t esp_iris_rpc_register(unsigned service, unsigned method,
                                mock_rpc_handler handler, void *context);
