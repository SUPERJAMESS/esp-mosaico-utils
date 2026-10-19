"""Common identity metadata survives reuse without changing API/auth fields."""
from pathlib import Path
import subprocess


def test_common_http_headers(tmp_path):
    component = Path(__file__).resolve().parents[1] / "firmware/vibe-mode/components/app_http_client"
    for name in ("esp_http_client.h", "esp_iris.h"):
        (tmp_path / name).write_text('#include "sdk.h"\n')
    (tmp_path / "sdk.h").write_text(r'''
#pragma once
#include <assert.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_STATE 4
typedef struct {
    char identity[33];
    const char *authorization, *url, *body;
} client_t;
typedef client_t *esp_http_client_handle_t;
static int identity_error, header_error;
static const char *identity = "4553502d495249530100001122334455";
static int esp_iris_format_device_id(char out[33])
{ if (identity_error) return identity_error; strcpy(out, identity); return ESP_OK; }
static int esp_http_client_set_header(client_t *c, const char *key, const char *value)
{
    assert(!strcmp(key, "X-Device-ID") && strlen(value) == 32);
    if (header_error) return header_error;
    strcpy(c->identity, value);
    return ESP_OK;
}
static int esp_http_client_delete_header(client_t *c, const char *key)
{
    assert(!strcmp(key, "X-Device-ID"));
    if (header_error) return header_error;
    if (!c->identity[0]) return ESP_ERR_NOT_FOUND;
    c->identity[0] = 0;
    return ESP_OK;
}
''')
    (tmp_path / "test.c").write_text(r'''
#include "factory_http_client.c"
int main(void)
{
    client_t client = {.authorization="Bearer token", .url="https://example.test/path?api=v1", .body="{}"};
    assert(factory_http_client_set_common_headers(NULL) == ESP_ERR_INVALID_ARG);
    identity_error = ESP_ERR_INVALID_STATE;
    assert(factory_http_client_set_common_headers(&client) == ESP_OK && !client.identity[0]);
    identity_error = ESP_OK;
    assert(factory_http_client_set_common_headers(&client) == ESP_OK);
    assert(!strcmp(client.identity, identity));
    assert(factory_http_client_set_common_headers(&client) == ESP_OK);
    identity = "4553502d495249530100aabbccddeeff";
    assert(factory_http_client_set_common_headers(&client) == ESP_OK && !strcmp(client.identity, identity));
    identity_error = ESP_ERR_INVALID_STATE;
    assert(factory_http_client_set_common_headers(&client) == ESP_OK && !client.identity[0]);
    header_error = ESP_ERR_NO_MEM;
    assert(factory_http_client_set_common_headers(&client) == ESP_ERR_NO_MEM);
    identity_error = ESP_OK;
    assert(factory_http_client_set_common_headers(&client) == ESP_ERR_NO_MEM);
    assert(!strcmp(client.authorization, "Bearer token"));
    assert(!strcmp(client.url, "https://example.test/path?api=v1"));
    assert(!strcmp(client.body, "{}"));
    return 0;
}
''')
    executable = tmp_path / "headers"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(tmp_path),
                    "-I" + str(component / "include"), "-I" + str(component / "src"),
                    str(tmp_path / "test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
