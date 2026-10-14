#include "app_iris_ui.h"

#include <string.h>
#include "esp_iris.h"
#include "esp_iris_service_profiles.h"

typedef struct {
    app_iris_ui_t ui;
} input_context_t;
static input_context_t s_input;

/* Same 12-byte little-endian pointer contract as the Gateway input adapter. */
#define POINTER_SERVICE_ID ESP_IRIS_POINTER_SERVICE_ID
#define POINTER_METHOD_ID ESP_IRIS_POINTER_METHOD_ID
#define POINTER_MESSAGE_SIZE ESP_IRIS_POINTER_MESSAGE_SIZE

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                             size_t response_capacity, size_t *response_size, void *user_ctx)
{
    input_context_t *ctx = user_ctx;
    esp_iris_status_t status = {0};
    esp_err_t err = esp_iris_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }
    if (!status.session_ready || status.transport != ESP_IRIS_TRANSPORT_KIND_USB) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (request == NULL || request->payload == NULL ||
        request->payload_size != POINTER_MESSAGE_SIZE || response == NULL ||
        response_size == NULL || response_capacity < POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *message = request->payload;
    const int16_t x = (int16_t)(message[2] | ((uint16_t)message[3] << 8));
    const int16_t y = (int16_t)(message[4] | ((uint16_t)message[5] << 8));
    if (message[0] > 2 || message[1] != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    err = ctx->ui.touch(ctx->ui.ctx, x, y, message[0] != 2);
    if (err != ESP_OK)
        return err;
    memcpy(response, message, POINTER_MESSAGE_SIZE);
    *response_size = POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

esp_err_t app_iris_input_register(const app_iris_ui_t *ui)
{
    if (ui == NULL || ui->touch == NULL)
        return ESP_ERR_INVALID_ARG;
    s_input.ui = *ui;
    return esp_iris_rpc_register(POINTER_SERVICE_ID, POINTER_METHOD_ID, pointer_rpc, &s_input);
}
