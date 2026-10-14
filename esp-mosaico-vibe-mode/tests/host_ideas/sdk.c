// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "app_storage.h"
#include "factory_ideas.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static mock_environment_t env = {.network = true, .release = "v1"};
static void (*queued_task)(void *);
static void *queued_arg;
static int64_t clock_us;
static unsigned char flash[16 * 1024 * 1024];
static unsigned char blob[1024], staged_blob[1024];
static size_t blob_size, staged_size;
static esp_partition_t boot;
struct mock_mutex {
    bool locked;
};
struct mock_http {
    char file[128], body[2048];
    size_t expected_body, body_size, size, cursor, readable;
    unsigned char *data;
    bool post, content_type, closed;
    int fault;
};

mock_environment_t *mock_environment(void)
{
    return &env;
}
void mock_assert_idle(void)
{
    assert(!queued_task && !env.lease && env.http_live == 0);
}
void mock_run_worker(void)
{
    assert(queued_task);
    void (*task)(void *) = queued_task;
    void *arg = queued_arg;
    queued_task = NULL;
    queued_arg = NULL;
    task(arg);
    mock_assert_idle();
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(struct mock_mutex));
}
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout)
{
    if (mutex->locked) {
        assert(timeout == 0); /* A blocking recursive acquisition would deadlock on device. */
        return 0;
    }
    mutex->locked = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex->locked);
    mutex->locked = false;
    return pdTRUE;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle)
{
    (void)name;
    (void)stack;
    (void)priority;
    (void)handle;
    assert(!queued_task);
    if (env.fail_task) {
        env.fail_task = false;
        return 0;
    }
    queued_task = task;
    queued_arg = arg;
    return pdPASS;
}
void vTaskDelete(void *task)
{
    assert(task == NULL);
}
void vTaskDelay(unsigned ticks)
{
    clock_us += ticks * 1000;
}
int64_t esp_timer_get_time(void)
{
    return ++clock_us;
}
void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}
const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "injected/test error";
}
void esp_restart(void)
{
    env.restarts++;
}
esp_err_t app_storage_acquire(void)
{
    if (env.storage_busy || env.lease)
        return ESP_ERR_INVALID_STATE;
    env.lease = true;
    return ESP_OK;
}
void app_storage_release(void)
{
    assert(env.lease);
    env.lease = false;
}
esp_err_t factory_network_get_snapshot(factory_network_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = env.network ? FACTORY_NETWORK_CONNECTED : FACTORY_NETWORK_NO_CREDENTIALS;
    return ESP_OK;
}

/* Redirect only the firmware's fixed NAND mount into pytest's temporary
 * directory. No test needs /nand privileges or touches a consuming workspace. */
static const char *mapped(const char *path, char out[4096])
{
    if (!strncmp(path, "/nand", 5) && (path[5] == '/' || path[5] == 0)) {
        assert(env.mount_dir);
        int n = snprintf(out, 4096, "%s%s", env.mount_dir, path + 5);
        assert(n > 0 && n < 4096);
        return out;
    }
    return path;
}
FILE *__real_fopen(const char *, const char *);
int __real_stat(const char *, struct stat *);
DIR *__real_opendir(const char *);
int __real_mkdir(const char *, mode_t);
int __real_unlink(const char *);
int __real_rmdir(const char *);
int __real_rename(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    char out[4096];
    if (!strncmp(path, "/nand/", 6) && strpbrk(mode, "wa+"))
        env.writes++;
    return __real_fopen(mapped(path, out), mode);
}
int __wrap_stat(const char *path, struct stat *st)
{
    char out[4096];
    return __real_stat(mapped(path, out), st);
}
DIR *__wrap_opendir(const char *path)
{
    char out[4096];
    return __real_opendir(mapped(path, out));
}
int __wrap_mkdir(const char *path, mode_t mode)
{
    char out[4096];
    env.writes++;
    return __real_mkdir(mapped(path, out), mode);
}
int __wrap_unlink(const char *path)
{
    char out[4096];
    env.writes++;
    return __real_unlink(mapped(path, out));
}
int __wrap_rmdir(const char *path)
{
    char out[4096];
    env.writes++;
    return __real_rmdir(mapped(path, out));
}
int __wrap_rename(const char *from, const char *to)
{
    char a[4096], b[4096];
    env.writes++;
    return __real_rename(mapped(from, a), mapped(to, b));
}

esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(env.network);
    assert(!strncmp(config->url, "https://ideas.test/api/v1/ideas/", 30));
    assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
    struct mock_http *client = calloc(1, sizeof(*client));
    assert(client);
    const char *name = strrchr(config->url, '/') + 1;
    snprintf(client->file, sizeof(client->file), "%s", name);
    client->post = config->method == HTTP_METHOD_POST;
    if (!strcmp(name, "resolve")) {
        assert(client->post && !env.lease);
        env.resolves++;
    } else if (strstr(name, ".bin")) {
        assert(!client->post && env.lease);
        env.firmware++;
    } else {
        assert(!client->post && !env.lease);
        env.assets++;
    }
    client->fault = env.fault_file && !strcmp(env.fault_file, name) ? env.fault : MOCK_OK;
    env.http_live++;
    return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    assert(!strcmp(key, "Content-Type") && !strcmp(value, "application/json"));
    c->content_type = true;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length)
{
    assert(length >= 0 && length < sizeof(c->body));
    c->expected_body = length;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int size)
{
    int n = size > 13 ? 13 : size; /* Force the real POST send loop to retry partial writes. */
    assert(c->body_size + n <= c->expected_body);
    memcpy(c->body + c->body_size, data, n);
    c->body_size += n;
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    char path[4096];
    if (c->post) {
        assert(c->content_type && c->body_size == c->expected_body);
        cJSON *body = cJSON_Parse(c->body);
        assert(body);
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "code")), "042731"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "board_id")), "test-s31"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "locale")), "en"));
        assert(cJSON_GetObjectItem(body, "recovery_abi")->valueint == 2);
        assert(cJSON_GetObjectItem(body, "layout_version")->valueint == 5);
        assert(cJSON_GetObjectItem(body, "flash_size")->valueint == sizeof(flash));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "download_schema")),
                       "mosaico-ideas/download/1"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "display_schema")),
                       "mosaico-ideas/display/1"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "font_format")), "MIFF/1"));
        if (cJSON_GetObjectItem(body, "application_id")) {
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "application_id")),
                           "stable-app-id"));
            assert(cJSON_IsString(cJSON_GetObjectItem(body, "current_release_id")));
            env.requests_with_identity++;
        }
        cJSON_Delete(body);
        snprintf(path, sizeof(path), "%s/%s.json", env.fixture_dir, env.release);
    } else {
        snprintf(path, sizeof(path), "%s/%s", env.fixture_dir, c->file);
    }
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    assert(length > 0);
    rewind(f);
    c->size = c->readable = length;
    c->data = malloc(c->size);
    assert(c->data && fread(c->data, 1, c->size, f) == c->size);
    assert(fclose(f) == 0);
    if (c->fault == MOCK_CORRUPT)
        c->data[0] ^= 1;
    if (c->fault == MOCK_TRUNCATE)
        c->readable /= 2;
    return c->size;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c->fault == MOCK_NOT_FOUND ? 404 : c->fault == MOCK_REDIRECT ? 302 : 200;
}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c)
{
    return c->size + (c->fault == MOCK_WRONG_LENGTH ? 1 : 0);
}
int esp_http_client_read(esp_http_client_handle_t c, char *data, int size)
{
    if (env.cancel_on_read && !strcmp(env.cancel_on_read, c->file)) {
        env.cancel_on_read = NULL;
        assert(factory_ideas_cancel() == ESP_OK);
        env.cancels++;
    }
    if (c->fault == MOCK_READ_ERROR)
        return -1;
    size_t n = size;
    if (n > 37)
        n = 37;
    if (n > c->readable - c->cursor)
        n = c->readable - c->cursor;
    memcpy(data, c->data + c->cursor, n);
    c->cursor += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    return c->cursor == c->size;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    assert(!c->closed);
    c->closed = true;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    assert(c->closed && env.http_live);
    env.http_live--;
    free(c->data);
    free(c);
    return ESP_OK;
}

int psa_crypto_init(void)
{
    return PSA_SUCCESS;
}
int psa_hash_setup(psa_hash_operation_t *hash, int algorithm)
{
    assert(algorithm == PSA_ALG_SHA_256);
    return SHA256_Init(hash) ? PSA_SUCCESS : -1;
}
int psa_hash_update(psa_hash_operation_t *hash, const uint8_t *data, size_t size)
{
    return SHA256_Update(hash, data, size) ? PSA_SUCCESS : -1;
}
int psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *size)
{
    assert(capacity >= 32);
    *size = 32;
    return SHA256_Final(out, hash) ? PSA_SUCCESS : -1;
}
int psa_hash_abort(psa_hash_operation_t *hash)
{
    (void)hash;
    return PSA_SUCCESS;
}
int psa_hash_compute(int algorithm, const uint8_t *data, size_t size, uint8_t *out, size_t capacity,
                     size_t *written)
{
    assert(algorithm == PSA_ALG_SHA_256 && capacity >= 32);
    *written = 32;
    return SHA256(data, size, out) ? PSA_SUCCESS : -1;
}
esp_err_t esp_flash_get_size(void *chip, uint32_t *size)
{
    (void)chip;
    *size = sizeof(flash);
    return ESP_OK;
}
esp_err_t esp_flash_read(void *chip, void *out, uint32_t offset, size_t size)
{
    (void)chip;
    assert((uint64_t)offset + size <= sizeof(flash));
    memcpy(out, flash + offset, size);
    return ESP_OK;
}
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return &boot;
}
void mock_corrupt_flash(void)
{
    flash[0x210000] ^= 1;
}
esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode,
                                  nvs_handle_t *out)
{
    assert(!strcmp(partition, "sysmeta") && !strcmp(ns, "ideas"));
    *out = mode + 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == NVS_READONLY + 1 && !strcmp(key, "boot"));
    if (!blob_size)
        return ESP_ERR_NOT_FOUND;
    assert(*size >= blob_size);
    *size = blob_size;
    memcpy(out, blob, blob_size);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == NVS_READWRITE + 1 && !strcmp(key, "boot") && size <= sizeof(blob));
    staged_size = size;
    memcpy(staged_blob, data, size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE + 1);
    if (env.fail_commit)
        return ESP_FAIL;
    memcpy(blob, staged_blob, staged_size);
    blob_size = staged_size;
    env.commits++;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

/* The NAND installer boundary copies verified fixture payloads into fake NOR.
 * Flash policy/commit gates are exercised by the separate backend fixture. */
esp_err_t factory_nand_install_idea(const char *manifest_path, const char *manifest,
                                    esp_err_t (*before_commit)(void *), void *ctx)
{
    assert(env.lease && before_commit && ctx);
    env.installs++;
    cJSON *json = cJSON_Parse(manifest);
    assert(json);
    cJSON *component;
    cJSON_ArrayForEach(component, cJSON_GetObjectItem(json, "components"))
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(component, "file"));
        uint32_t offset = cJSON_GetObjectItem(component, "target_offset")->valueint;
        size_t size = cJSON_GetObjectItem(component, "size")->valueint;
        char path[4096];
        snprintf(path, sizeof(path), "%.*s/%s", (int)(strrchr(manifest_path, '/') - manifest_path),
                 manifest_path, name);
        assert((uint64_t)offset + size <= sizeof(flash));
        FILE *f = fopen(path, "rb");
        assert(f && fread(flash + offset, 1, size, f) == size && fgetc(f) == EOF);
        fclose(f);
    }
    cJSON_Delete(json);
    esp_err_t err = before_commit(ctx);
    if (err == ESP_OK) {
        boot.address = 0x210000;
        env.flash_version = flash[boot.address];
    }
    return err;
}
