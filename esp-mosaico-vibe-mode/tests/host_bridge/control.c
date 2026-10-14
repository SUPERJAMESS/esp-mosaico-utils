#include "control_sdk.h"
#include <assert.h>
#include CONTROL_SOURCE
static mock_rpc_handler handlers[6];
static int opened;
static bool usb = true;
static iris_bridge_snapshot_t state = {.state = "IDLE"};
esp_err_t esp_iris_rpc_register(unsigned service, unsigned method,
                                mock_rpc_handler handler, void *context)
{
    (void)context;
    assert(service == 0x1202 && method < 6);
    handlers[method] = handler;
    return 0;
}
esp_err_t esp_iris_get_status(esp_iris_status_t *status)
{
    status->session_ready = true;
    status->transport = usb ? ESP_IRIS_TRANSPORT_KIND_USB : ESP_IRIS_TRANSPORT_KIND_TCP;
    return 0;
}
esp_err_t factory_network_connect(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return 0;
}
esp_err_t factory_network_get_snapshot(factory_network_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    return 0;
}
void iris_bridge_get_snapshot(iris_bridge_snapshot_t *out)
{
    *out = state;
}
esp_err_t factory_ui_open_bridge(void *ctx)
{
    opened++;
    strcpy(state.state, "REGISTERING");
    state.running = true;
    return 0;
}
int main(void)
{
    const app_iris_ui_t ui = {.open_download = factory_ui_open_bridge};
    assert(factory_recovery_control_register(&ui) == 0);
    assert(!handlers[3]);
    assert(handlers[4] && handlers[5]);
    esp_iris_rpc_request_t request = {0};
    uint8_t buffer[1024];
    size_t size = 0;
    assert(handlers[5](&request, buffer, sizeof(buffer), &size, &s_control) == 0);
    assert(!opened);
    assert(strstr((char *)buffer, "IDLE"));
    usb = false;
    assert(handlers[4](&request, buffer, sizeof(buffer), &size, &s_control) ==
           ESP_ERR_NOT_ALLOWED);
    assert(handlers[5](&request, buffer, sizeof(buffer), &size, &s_control) ==
           ESP_ERR_NOT_ALLOWED);
    assert(!opened);
    usb = true;
    request.payload_size = 1;
    assert(handlers[4](&request, buffer, sizeof(buffer), &size, &s_control) ==
           ESP_ERR_INVALID_SIZE);
    assert(!opened);
    request.payload_size = 0;
    assert(handlers[4](&request, buffer, sizeof(buffer), &size, &s_control) == 0);
    assert(opened == 1);
    strcpy(state.code, "ABCDE-12345");
    strcpy(state.server_url, "https://flash.example.com");
    strcpy(state.state, "PAIRING");
    assert(handlers[5](&request, buffer, sizeof(buffer), &size, &s_control) == 0);
    assert(opened == 1);
    cJSON *json = cJSON_Parse((char *)buffer);
    assert(json);
    assert(!cJSON_GetObjectItem(json, "token"));
    assert(!strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "ABCDE-12345"));
    cJSON_Delete(json);
    assert(handlers[5](&request, buffer, 8, &size, &s_control) == ESP_ERR_INVALID_SIZE);
    puts("USB-only Bridge control and side-effect-free status passed");
}
