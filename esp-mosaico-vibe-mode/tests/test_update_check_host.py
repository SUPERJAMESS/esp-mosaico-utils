"""Exercise release parsing, bounded HTTP reception and reconnect coalescing."""
import os
from pathlib import Path
import re
import subprocess


def test_update_check(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = root / "firmware/vibe-mode/components/app_update_check/src/factory_update_check.c"
    http_client = root / "firmware/vibe-mode/components/app_http_client"
    cjson = Path(os.environ.get("CJSON_COMPONENT_DIR", str(root / "firmware/vibe-mode/managed_components/espressif__cjson")))
    header = next(cjson.rglob("cJSON.h"))
    for name in re.findall(r'#include "([^"]+)"', source.read_text()) + ["esp_iris.h"]:
        if name in {"factory_update_check.h", "factory_http_client.h", "cJSON.h"}:
            continue
        stub = tmp_path / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('#include "sdk.h"\n')
    (tmp_path / "sdk.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define strlcpy mock_strlcpy
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_ARG 5
#define ESP_ERR_NOT_FOUND 6
#define CONFIG_IRIS_FACTORY_UPDATE_PRODUCT "esp-mosaico-vibe-mode"
#define CONFIG_IRIS_FACTORY_UPDATE_MANIFEST_URL "https://example.com/vibe.json"
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdPASS 1
static int creates, create_result = pdPASS;
static int64_t now = 100;
static int64_t esp_timer_get_time(void) { return now; }
static int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle)
{ ++creates; return create_result; }
static void vTaskDelete(void *task) {}
typedef struct { char version[32]; } esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void)
{ static esp_app_desc_t desc = {"0.2.0"}; return &desc; }
static size_t strlcpy(char *dst, const char *src, size_t n)
{ size_t len = strlen(src); if (n) snprintf(dst, n, "%s", src); return len; }
typedef struct { int event_id, data_len; void *user_data; char *data; } esp_http_client_event_t;
#define HTTP_EVENT_ON_DATA 1
typedef void *esp_http_client_handle_t;
static void esp_crt_bundle_attach(void) {}
typedef struct {
    const char *url, *user_agent;
    void (*crt_bundle_attach)(void);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
} esp_http_client_config_t;
static bool http_available, identity_unavailable, device_header;
static int performs, header_error;
static esp_http_client_config_t http_config;
static void *esp_http_client_init(const esp_http_client_config_t *config)
{ http_config = *config; device_header = false; return http_available ? &http_config : NULL; }
static int esp_iris_format_device_id(char out[33])
{ strcpy(out, "4553502d495249530100001122334455"); return identity_unavailable ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
static int esp_http_client_set_header(void *c, const char *k, const char *v)
{
    if (!strcmp(k, "X-Device-ID")) {
        assert(!strcmp(v, "4553502d495249530100001122334455"));
        if (header_error) return header_error;
        device_header = true;
    }
    return ESP_OK;
}
static int esp_http_client_delete_header(void *c, const char *key)
{ assert(!strcmp(key, "X-Device-ID")); device_header = false; return ESP_ERR_NOT_FOUND; }
static int esp_http_client_perform(void *c)
{
    assert(device_header == !identity_unavailable);
    ++performs;
    char body[] = "{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"0.2.1\"}";
    esp_http_client_event_t event = {.event_id=HTTP_EVENT_ON_DATA, .data_len=sizeof(body)-1,
        .data=body, .user_data=http_config.user_data};
    return http_config.event_handler(&event);
}
static int esp_http_client_get_status_code(void *c) { return 200; }
static bool esp_http_client_is_complete_data_received(void *c) { return true; }
static void esp_http_client_cleanup(void *c) {}
''')
    (tmp_path / "test.c").write_text(r'''
#include <assert.h>
#include "factory_http_client.c"
#include "factory_update_check.c"
static void check(const char *json, int expected, bool available)
{
    response_t response = {0};
    factory_update_check_snapshot_t result = {0};
    snprintf(response.body, sizeof(response.body), "%s", json);
    response.length = strlen(json);
    assert(parse_manifest(&response, &result) == expected);
    if (expected == ESP_OK) assert(result.available == available);
}

static void metadata(const char *file, const char *size, const char *hash, int expected)
{
    response_t response = {0};
    factory_update_check_snapshot_t result = {0};
    response.length = snprintf(response.body, sizeof(response.body),
        "{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"0.2.1\","
        "\"firmware\":{\"file\":\"%s\",\"size\":%s,\"sha256\":\"%s\"}}", file, size, hash);
    assert(parse_manifest(&response, &result) == expected);
    if (expected == ESP_OK) assert(result.available && result.installable && result.size == 512);
}
int main(void)
{
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    metadata("esp-mosaico-vibe-mode-0.2.1.bin", "512", sha, ESP_OK);
    metadata("../firmware.bin", "512", sha, ESP_ERR_INVALID_RESPONSE);
    metadata("https://other.test/a.bin", "512", sha, ESP_ERR_INVALID_RESPONSE);
    metadata("firmware.bin?download", "512", sha, ESP_ERR_INVALID_RESPONSE);
    metadata("firmware.bin", "512.5", sha, ESP_ERR_INVALID_RESPONSE);
    metadata("firmware.bin", "1835009", sha, ESP_ERR_INVALID_RESPONSE);
    metadata("firmware.bin", "512", "bad hash", ESP_ERR_INVALID_RESPONSE);

    check("{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"0.2.1\"}", ESP_OK, true);
    check("{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"0.2.0\"}", ESP_OK, false);
    check("{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"0.1.9\"}", ESP_OK, false);
    check("{\"schema_version\":1,\"product\":\"esp-mosaico\",\"latest_version\":\"9.0.0\"}", ESP_ERR_INVALID_RESPONSE, false);
    check("{\"schema_version\":1,\"product\":\"esp-mosaico-vibe-mode\",\"latest_version\":\"invalid\"}", ESP_ERR_INVALID_RESPONSE, false);
    check("[]", ESP_ERR_INVALID_RESPONSE, false);
    check("{}garbage", ESP_ERR_INVALID_RESPONSE, false);
    check("{", ESP_ERR_INVALID_RESPONSE, false);
    response_t response = {.length = MANIFEST_MAX_BYTES};
    esp_http_client_event_t event = {.event_id = HTTP_EVENT_ON_DATA, .data_len = 1, .data = "x", .user_data = &response};
    assert(receive(&event) == ESP_ERR_INVALID_SIZE);
    response.length = 0;
    assert(receive(&event) == ESP_ERR_INVALID_SIZE);
    factory_update_check_request(false);
    factory_update_check_request(false);
    assert(creates == 1);
    s_running = false;
    factory_update_check_request(false);
    assert(creates == 1);
    now += CHECK_INTERVAL_US;
    factory_update_check_request(false);
    assert(creates == 2);
    s_snapshot.available = true;
    check_task(NULL);
    assert(s_snapshot.available && !s_running && s_snapshot.error == ESP_ERR_NO_MEM);
    uint32_t generation = s_snapshot.generation;
    factory_update_check_request(true);
    factory_update_check_request(true);
    assert(creates == 3);
    factory_update_check_snapshot_t snapshot;
    factory_update_check_snapshot(&snapshot);
    assert(snapshot.checking && snapshot.generation == generation);
    factory_update_check_snapshot_t fresh = {.available = false};
    check_complete(&fresh, ESP_OK);
    factory_update_check_snapshot(&snapshot);
    assert(!snapshot.checking && !snapshot.available && !snapshot.error && snapshot.generation == generation + 1);
    create_result = 0;
    factory_update_check_request(true);
    factory_update_check_snapshot(&snapshot);
    assert(!snapshot.checking && snapshot.error == ESP_ERR_NO_MEM && snapshot.generation == generation + 2);
    http_available = true;
    check_task(NULL);
    assert(performs == 1 && s_snapshot.available && s_snapshot.error == ESP_OK);
    identity_unavailable = true;
    check_task(NULL);
    assert(performs == 2 && s_snapshot.error == ESP_OK);
    identity_unavailable = false;
    header_error = ESP_ERR_NO_MEM;
    check_task(NULL);
    assert(performs == 2 && s_snapshot.error == ESP_ERR_NO_MEM);
    return 0;
}
''')
    executable = tmp_path / "test"
    subprocess.run(["cc", "-std=gnu11", "-Werror=implicit-function-declaration",
                    "-I" + str(tmp_path), "-I" + str(source.parent), "-I" + str(source.parent.parent / "include"), "-I" + str(header.parent),
                    "-I" + str(http_client / "include"), "-I" + str(http_client / "src"),
                    str(tmp_path / "test.c"), str(header.with_name("cJSON.c")), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
