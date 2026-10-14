// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "app_storage.h"
#include "cJSON.h"
#include "factory_ideas.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static mock_environment_t env = {.network = true, .release = "v1"};
struct mock_queue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void *item;
    bool enabled, running, stop;
};
struct mock_task { pthread_t thread; void (*entry)(void *); void *arg; };
static struct mock_task worker;
static QueueHandle_t jobs;
static int64_t clock_us;
static unsigned char flash[16 * 1024 * 1024];
static unsigned char blob[1024], staged_blob[1024];
static size_t blob_size, staged_size;
static esp_partition_t boot;
struct mock_mutex {
    bool locked;
};
struct mock_http {
    char file[128], alias[128], body[2048];
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    size_t expected_body, body_size, size, cursor, readable;
    unsigned char *data;
    bool post, content_type, closed;
    char locale[6];
    int fault;
};

mock_environment_t *mock_environment(void) { return &env; }
void mock_assert_idle(void) {
    assert(jobs && !jobs->running && !jobs->enabled && !jobs->item);
    assert(!env.lease && env.http_live == 0);
}
void mock_run_one_job(void) {
    assert(jobs);
    pthread_mutex_lock(&jobs->mutex);
    assert(jobs->item && !jobs->enabled && !jobs->running);
    jobs->enabled = true;
    pthread_cond_broadcast(&jobs->changed);
    while (jobs->enabled || jobs->running)
        pthread_cond_wait(&jobs->changed, &jobs->mutex);
    pthread_mutex_unlock(&jobs->mutex);
}
void mock_run_worker(void) {
    mock_run_one_job();
    mock_assert_idle();
}
void mock_stop_worker(void) {
    mock_assert_idle();
    pthread_mutex_lock(&jobs->mutex);
    jobs->stop = true;
    pthread_cond_broadcast(&jobs->changed);
    pthread_mutex_unlock(&jobs->mutex);
    assert(pthread_join(worker.thread, NULL) == 0);
    assert(env.tasks_created == 1 && env.tasks_entered == 1);
}
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
    if (env.fail_init_step == 1) {
        env.fail_init_step = 0;
        return NULL;
    }
    return __real_calloc(count, size);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    if (env.fail_init_step == 2) return NULL;
    ++env.mutex_live;
    return calloc(1, sizeof(struct mock_mutex));
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) {
    assert(mutex && !mutex->locked && env.mutex_live);
    --env.mutex_live;
    free(mutex);
}
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout) {
    if (mutex->locked) {
        assert(timeout == 0); /* A blocking recursive acquisition would deadlock on device. */
        return 0;
    }
    mutex->locked = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(mutex->locked);
    mutex->locked = false;
    static bool observing;
    if (env.observe && !observing) {
        observing = true;
        env.observe();
        observing = false;
    }
    return pdTRUE;
}
QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    assert(count == 1 && size == sizeof(void *));
    if (env.fail_init_step == 3) return NULL;
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    assert(queue && pthread_mutex_init(&queue->mutex, NULL) == 0);
    assert(pthread_cond_init(&queue->changed, NULL) == 0);
    ++env.queue_live;
    jobs = queue;
    return queue;
}
void vQueueDelete(QueueHandle_t queue) {
    assert(queue && env.queue_live && !env.tasks_created);
    assert(pthread_mutex_destroy(&queue->mutex) == 0);
    assert(pthread_cond_destroy(&queue->changed) == 0);
    free(queue);
    jobs = NULL;
    --env.queue_live;
}
int xQueueSend(QueueHandle_t queue, const void *item, unsigned timeout) {
    assert(queue && timeout == 0);
    if (env.fail_send || env.fail_init_step == 5) {
        env.fail_send = false;
        return 0;
    }
    pthread_mutex_lock(&queue->mutex);
    int ok = !queue->item;
    if (ok) memcpy(&queue->item, item, sizeof(queue->item));
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex);
    return ok;
}
int xQueueReceive(QueueHandle_t queue, void *item, unsigned timeout) {
    assert(queue && timeout == portMAX_DELAY);
    pthread_mutex_lock(&queue->mutex);
    if (queue->running) {
        queue->running = queue->enabled = false;
        ++env.jobs_finished;
        pthread_cond_broadcast(&queue->changed);
    }
    while (!queue->stop && (!queue->enabled || !queue->item))
        pthread_cond_wait(&queue->changed, &queue->mutex);
    if (queue->stop) {
        pthread_mutex_unlock(&queue->mutex);
        pthread_exit(NULL);
    }
    memcpy(item, &queue->item, sizeof(queue->item));
    queue->item = NULL;
    queue->running = true;
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}
static void *worker_entry(void *arg) {
    (void)arg;
    ++env.tasks_entered;
    worker.entry(worker.arg); /* The production task must stay in its receive loop. */
    assert(false);
    return NULL;
}
int xTaskCreatePinnedToCoreWithCaps(void (*task)(void *), const char *name, unsigned stack,
                                  void *arg, unsigned priority, TaskHandle_t *handle,
                                  int core, unsigned caps) {
    assert(!env.deny_new_tasks && !env.tasks_created);
    assert(!strcmp(name, "ideas") && stack == 16384 && priority == 3 && core == tskNO_AFFINITY);
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    if (env.fail_init_step == 4) return 0;
    worker.entry = task;
    worker.arg = arg;
    *handle = &worker;
    ++env.tasks_created;
    assert(pthread_create(&worker.thread, NULL, worker_entry, NULL) == 0);
    return pdPASS;
}
void vTaskDelay(unsigned ticks) { clock_us += ticks * 1000; }
int64_t esp_timer_get_time(void) { return ++clock_us; }
void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}
const char *esp_err_to_name(esp_err_t err) {
    (void)err;
    return "injected/test error";
}
void esp_restart(void) { env.restarts++; }
esp_err_t app_storage_acquire(void) {
    if (env.storage_busy || env.lease)
        return ESP_ERR_INVALID_STATE;
    env.lease = true;
    return ESP_OK;
}
void app_storage_release(void) {
    assert(env.lease);
    env.lease = false;
}
esp_err_t factory_network_get_snapshot(factory_network_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->state = env.network ? FACTORY_NETWORK_CONNECTED : FACTORY_NETWORK_NO_CREDENTIALS;
    return ESP_OK;
}

/* Redirect only the firmware's fixed NAND mount into pytest's temporary
 * directory. No test needs /nand privileges or touches a consuming workspace.
 */
static const char *mapped(const char *path, char out[4096]) {
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
FILE *__wrap_fopen(const char *path, const char *mode) {
    char out[4096];
    if (!strncmp(path, "/nand/", 6) && strpbrk(mode, "wa+"))
        env.writes++;
    if (!strncmp(path, "/nand/", 6) && strstr(path, "/firmware/app.bin") && !strcmp(mode, "rb"))
        env.firmware_reads++;
    return __real_fopen(mapped(path, out), mode);
}
int __wrap_stat(const char *path, struct stat *st) {
    char out[4096];
    return __real_stat(mapped(path, out), st);
}
int __real_lstat(const char *, struct stat *);
int __real_open(const char *, int, ...);
int __wrap_lstat(const char *path, struct stat *st) {
    char out[4096];
    return __real_lstat(mapped(path, out), st);
}
int __wrap_open(const char *path, int flags, ...) {
    char out[4096];
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = va_arg(ap, int);
        va_end(ap);
        return __real_open(mapped(path, out), flags, mode);
    }
    return __real_open(mapped(path, out), flags);
}
DIR *__wrap_opendir(const char *path) {
    char out[4096];
    return __real_opendir(mapped(path, out));
}
int __wrap_mkdir(const char *path, mode_t mode) {
    char out[4096];
    env.writes++;
    return __real_mkdir(mapped(path, out), mode);
}
int __wrap_unlink(const char *path) {
    char out[4096];
    env.writes++;
    return __real_unlink(mapped(path, out));
}
int __wrap_rmdir(const char *path) {
    char out[4096];
    env.writes++;
    return __real_rmdir(mapped(path, out));
}
int __wrap_rename(const char *from, const char *to) {
    char a[4096], b[4096];
    env.writes++;
    return __real_rename(mapped(from, a), mapped(to, b));
}

esp_err_t esp_crt_bundle_attach(void *config) {
    (void)config;
    return ESP_OK;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(env.network);
    assert(!strncmp(config->url, "https://ideas.test/api/v1/ideas/", 30));
    assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
    struct mock_http *client = calloc(1, sizeof(*client));
    assert(client);
    client->event_handler = config->event_handler;
    assert(config->method == HTTP_METHOD_GET);
    if (strstr(config->url, "/042731/manifest.json?")) {
        assert(strstr(config->url, "profile=esp-mosaico%2Fv2&chip_id=32&flash_size="
                                   "16777216&recovery_version=0.2.0"));
        assert(!env.lease);
        strcpy(client->file, "resolve");
        strcpy(client->alias, "resolve");
        env.resolves++;
        strcpy(client->locale, strstr(config->url, "&locale=zh-CN") ? "zh-CN" : "en");
        assert(strstr(config->url, "&locale="));
    } else {
        const char *sha = strrchr(config->url, '/') + 1;
        assert(strlen(sha) == 64);
        strcpy(client->file, sha);
        /* Resolve aliases only inside this fault-injection fixture. Requests
         * themselves must use the content hash from the pinned snapshot. */
        const char *releases[] = {env.release, "v1", "v2"};
        bool found = false;
        for (unsigned r = 0; r < 3 && !found; ++r) {
            char path[4096], raw[8192];
            snprintf(path, sizeof(path), "%s/%s-index.json", env.fixture_dir, releases[r]);
            FILE *f = fopen(path, "rb");
            assert(f);
            size_t n = fread(raw, 1, sizeof(raw) - 1, f);
            raw[n] = 0;
            fclose(f);
            cJSON *index = cJSON_Parse(raw), *item = cJSON_GetObjectItem(index, sha);
            if (item) {
                snprintf(client->alias, sizeof(client->alias), "%s",
                         cJSON_GetStringValue(cJSON_GetObjectItem(item, "alias")));
                if (cJSON_IsTrue(cJSON_GetObjectItem(item, "firmware"))) {
                    assert(env.lease);
                    env.firmware++;
                } else
                    env.assets++;
                found = true;
            }
            cJSON_Delete(index);
        }
        assert(found);
    }
    client->fault = env.fault_file && (!strcmp(env.fault_file, client->alias) ||
        (!strcmp(client->alias, "resolve") && !strcmp(env.fault_file, "display.json"))) ? env.fault : MOCK_OK;
    if (client->fault != MOCK_OK) {
        ++env.fault_hits;
        if (env.fault_limit && env.fault_hits > env.fault_limit)
            client->fault = MOCK_OK;
    }
    env.http_live++;
    return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key,
                                     const char *value) {
    if (!strcmp(key, "Accept")) {
        assert(!strcmp(value, "multipart/related") && !strcmp(c->file, "resolve"));
        return ESP_OK;
    }
    assert(!strcmp(key, "Accept-Encoding") && !strcmp(value, "identity"));
    c->content_type = true;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length) {
    assert(length >= 0 && length < sizeof(c->body));
    c->expected_body = length;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int size) {
    int n = size > 13 ? 13 : size; /* Force the real POST send loop to retry partial writes. */
    assert(c->body_size + n <= c->expected_body);
    memcpy(c->body + c->body_size, data, n);
    c->body_size += n;
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    char path[4096];
    if (!strcmp(c->file, "resolve"))
        snprintf(path, sizeof(path), "%s/%s-%s.multipart", env.fixture_dir, env.release, c->locale);
    else
        snprintf(path, sizeof(path), "%s/%s", env.fixture_dir, c->file);
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    assert(length > 0);
    rewind(f);
    c->size = c->readable = length;
    c->data = malloc(c->size);
    assert(c->data && fread(c->data, 1, c->size, f) == c->size);
    assert(fclose(f) == 0);
    unsigned char digest[32];
    SHA256(c->data, c->size, digest);
    char etag[68] = "\"";
    for (unsigned i = 0; i < 32; ++i)
        snprintf(etag + 1 + i * 2, 3, "%02x", digest[i]);
    etag[65] = '"';
    etag[66] = 0;
    if (c->fault == MOCK_ETAG) etag[1] = etag[1] == '0' ? '1' : '0';
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_HEADER, .header_key = "ETag", .header_value = etag};
    c->event_handler(&event);
    event.header_key = "Content-Type";
    char media[192] = "application/octet-stream";
    if (!strcmp(c->file, "resolve")) {
        snprintf(path, sizeof(path), "%s/%s-%s.media", env.fixture_dir, env.release, c->locale);
        f = fopen(path, "rb");
        assert(f);
        size_t n = fread(media, 1, sizeof(media)-1, f);
        media[n] = 0;
        fclose(f);
    }
    event.header_value = media;
    c->event_handler(&event);
    if (c->fault == MOCK_CORRUPT)
        c->data[0] ^= 1;
    if (c->fault == MOCK_TRUNCATE)
        c->readable /= 2;
    return c->size;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return c->fault == MOCK_SERVICE_UNAVAILABLE ? 503
           : c->fault == MOCK_NOT_FOUND         ? 404
           : c->fault == MOCK_REDIRECT          ? 302
                                                : 200;
}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) {
    return c->size + (c->fault == MOCK_WRONG_LENGTH ? 1 : 0);
}
int esp_http_client_read(esp_http_client_handle_t c, char *data, int size) {
    if (env.cancel_on_read && (!strcmp(env.cancel_on_read, c->alias) ||
        (!strcmp(c->alias, "resolve") && !strcmp(env.cancel_on_read, "display.json") && c->cursor > c->size / 2))) {
        env.cancel_on_read = NULL;
        assert(factory_ideas_cancel() == ESP_OK);
        env.cancels++;
    }
    if (c->fault == MOCK_READ_ERROR && c->cursor > c->size / 2)
        return -1;
    size_t n = size;
    size_t limit = env.read_chunk ? env.read_chunk : 37;
    if (n > limit)
        n = limit;
    if (n > c->readable - c->cursor)
        n = c->readable - c->cursor;
    memcpy(data, c->data + c->cursor, n);
    c->cursor += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    return c->cursor == c->size;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    assert(!c->closed);
    c->closed = true;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(c->closed && env.http_live);
    env.http_live--;
    free(c->data);
    free(c);
    return ESP_OK;
}

int psa_crypto_init(void) { return PSA_SUCCESS; }
int psa_hash_setup(psa_hash_operation_t *hash, int algorithm) {
    assert(algorithm == PSA_ALG_SHA_256);
    return SHA256_Init(hash) ? PSA_SUCCESS : -1;
}
int psa_hash_update(psa_hash_operation_t *hash, const uint8_t *data, size_t size) {
    return SHA256_Update(hash, data, size) ? PSA_SUCCESS : -1;
}
int psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *size) {
    assert(capacity >= 32);
    *size = 32;
    return SHA256_Final(out, hash) ? PSA_SUCCESS : -1;
}
int psa_hash_abort(psa_hash_operation_t *hash) {
    (void)hash;
    return PSA_SUCCESS;
}
int psa_hash_compute(int algorithm, const uint8_t *data, size_t size, uint8_t *out, size_t capacity,
                     size_t *written) {
    assert(algorithm == PSA_ALG_SHA_256 && capacity >= 32);
    *written = 32;
    return SHA256(data, size, out) ? PSA_SUCCESS : -1;
}
esp_err_t esp_flash_get_size(void *chip, uint32_t *size) {
    (void)chip;
    *size = sizeof(flash);
    return ESP_OK;
}
esp_err_t esp_flash_read(void *chip, void *out, uint32_t offset, size_t size) {
    (void)chip;
    assert((uint64_t)offset + size <= sizeof(flash));
    env.flash_read_bytes += size;
    memcpy(out, flash + offset, size);
    return ESP_OK;
}
const esp_partition_t *esp_ota_get_boot_partition(void) { return &boot; }
void mock_corrupt_flash(void) { flash[0x210000] ^= 1; }
esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode,
                                  nvs_handle_t *out) {
    assert(!strcmp(partition, "sysmeta") && !strcmp(ns, "ideas"));
    *out = mode + 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == NVS_READONLY + 1 && !strcmp(key, "boot"));
    if (!blob_size)
        return ESP_ERR_NOT_FOUND;
    assert(*size >= blob_size);
    *size = blob_size;
    memcpy(out, blob, blob_size);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    assert(handle == NVS_READWRITE + 1 && !strcmp(key, "boot") && size <= sizeof(blob));
    staged_size = size;
    memcpy(staged_blob, data, size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == NVS_READWRITE + 1);
    if (env.fail_commit)
        return ESP_FAIL;
    memcpy(blob, staged_blob, staged_size);
    blob_size = staged_size;
    env.commits++;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

/* The NAND installer boundary copies verified fixture payloads into fake NOR.
 * Flash policy/commit gates are exercised by the separate backend fixture. */
esp_err_t factory_nand_install_idea(const char *manifest_path, const char *manifest,
                                    esp_err_t (*before_commit)(void *), void *ctx) {
    assert(env.lease && before_commit && ctx);
    env.installs++;
    cJSON *json = cJSON_Parse(manifest);
    assert(json);
    cJSON *component;
    cJSON_ArrayForEach(component, cJSON_GetObjectItem(json, "components")) {
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
        env.flash_version = flash[boot.address + 48];
    }
    return err;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout) {
    (void)client;
    assert(timeout > 0 && timeout <= 60000);
    return ESP_OK;
}
const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t description = {.version = "0.2.0"};
    return &description;
}
static unsigned language = 1;
unsigned factory_locale_get(void) { return language; }
esp_err_t factory_locale_set(unsigned value) {
    language = value;
    return ESP_OK;
}
bool factory_system_update_recovery_supports(const char *minimum) {
    return !*minimum || !strcmp(minimum, "0.2.0");
}

uint32_t esp_random(void) { return 123; }
