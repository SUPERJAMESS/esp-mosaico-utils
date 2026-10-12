// SPDX-License-Identifier: Apache-2.0
#include "factory_ideas.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include "factory_system_update.h"
#include "factory_system_metadata.h"
#include "factory_ui.h"
#include "factory_ui_input.h"
#include "ideas_presentation.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/nand/ideas"
#define IO_BYTES 4096
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
static SemaphoreHandle_t s_lock;
static EXT_RAM_BSS_ATTR factory_ideas_snapshot_t s_state;
typedef struct { void *data; size_t size; } icon_t;
static icon_t s_icons[IDEAS_MAX_ITEMS];
typedef struct { factory_ideas_command_t command; char code[7]; } job_t;
typedef struct {
    uint32_t schema, offset, size;
    char code[7], application_id[65], release_id[65], image[65], layout[65];
} boot_record_t;

static void message(const char *title, const char *detail)
{
    LOCK();
    snprintf(s_state.title, sizeof(s_state.title), "%s", title);
    snprintf(s_state.detail, sizeof(s_state.detail), "%s", detail);
    UNLOCK();
}
static void hex(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) { out[i*2] = digits[digest[i] >> 4]; out[i*2+1] = digits[digest[i] & 15]; }
    out[64] = 0;
}
static esp_err_t verify_file(const ideas_file_t *file, const char *path, void *ctx)
{
    (void)ctx;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != file->size)
        return ESP_ERR_INVALID_SIZE;
    FILE *f = fopen(path, "rb");
    uint8_t *buffer = malloc(IO_BYTES);
    if (!f || !buffer) { if (f) fclose(f); free(buffer); return ESP_ERR_NO_MEM; }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    size_t n, total = 0;
    while (err == ESP_OK && (n = fread(buffer, 1, IO_BYTES, f))) {
        total += n;
        if (total > file->size || psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) err = ESP_FAIL;
        vTaskDelay(1);
    }
    uint8_t digest[32]; size_t size = 0; char actual[65];
    if (err == ESP_OK && (ferror(f) || total != file->size ||
        psa_hash_finish(&hash, digest, sizeof(digest), &size) != PSA_SUCCESS || size != 32)) err = ESP_FAIL;
    if (err == ESP_OK) { hex(digest, actual); if (strcmp(actual, file->sha256)) err = ESP_ERR_INVALID_CRC; }
    psa_hash_abort(&hash); fclose(f); free(buffer);
    return err;
}
static esp_err_t flash_hash(uint32_t offset, uint32_t size, const char *expected)
{
    uint8_t *buffer = malloc(IO_BYTES);
    if (!buffer) return ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t err = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (uint32_t done = 0; err == ESP_OK && done < size;) {
        size_t n = size - done < IO_BYTES ? size - done : IO_BYTES;
        err = esp_flash_read(esp_flash_default_chip, buffer, offset + done, n);
        if (err == ESP_OK && psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) err = ESP_FAIL;
        done += n; vTaskDelay(1);
    }
    uint8_t digest[32]; size_t n; char actual[65];
    if (err == ESP_OK && (psa_hash_finish(&hash, digest, 32, &n) != PSA_SUCCESS || n != 32)) err = ESP_FAIL;
    if (err == ESP_OK) { hex(digest, actual); if (strcmp(expected, actual)) err = ESP_ERR_INVALID_CRC; }
    psa_hash_abort(&hash); free(buffer); return err;
}
static bool boot_record(boot_record_t *record)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "ideas", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(*record);
    esp_err_t err = nvs_get_blob(nvs, "boot", record, &size);
    nvs_close(nvs);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (err != ESP_OK || size != sizeof(*record) || record->schema != 1 ||
        !memchr(record->code, 0, sizeof(record->code)) || !ideas_code_valid(record->code) ||
        !memchr(record->application_id, 0, sizeof(record->application_id)) ||
        record->image[64] || record->layout[64] || !boot || boot->address != record->offset ||
        record->offset != 0x210000 || !record->size || record->size > 16U*1024U*1024U) return false;
    return flash_hash(0x8000, 4096, record->layout) == ESP_OK &&
           flash_hash(record->offset, record->size, record->image) == ESP_OK;
}
static esp_err_t save_boot(void *ctx)
{
    const ideas_package_t *p = ctx;
    boot_record_t record = {.schema = 1, .offset = p->application_offset, .size = p->application_size};
    strcpy(record.code, p->code); strcpy(record.application_id, p->application_id);
    strcpy(record.release_id, p->release_id); strcpy(record.image, p->application_sha256); strcpy(record.layout, p->layout_sha256);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "ideas", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "boot", &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs); return err;
}
static esp_err_t scan(void)
{
    DIR *dir = opendir(ROOT);
    if (!dir && errno != ENOENT) return ESP_FAIL;
    factory_idea_t *items = calloc(IDEAS_MAX_ITEMS, sizeof(*items));
    ideas_package_t *p = malloc(sizeof(*p));
    if (!items || !p) { if (dir) closedir(dir); free(items); free(p); return ESP_ERR_NO_MEM; }
    boot_record_t boot = {0}; bool has_boot = boot_record(&boot);
    icon_t icons[IDEAS_MAX_ITEMS] = {0};
    struct dirent *entry; size_t count = 0, invalid = 0, examined = 0;
    while (dir && examined++ < 64 && (entry = readdir(dir))) {
        if (!ideas_code_valid(entry->d_name)) continue;
        if (ideas_store_recover(ROOT, entry->d_name) != ESP_OK ||
            ideas_store_read(ROOT, entry->d_name, p) != ESP_OK) { invalid++; continue; }
        if (count == IDEAS_MAX_ITEMS) { invalid++; continue; }
        factory_idea_t *item = &items[count++];
        strcpy(item->code, p->code); strcpy(item->application_id, p->application_id);
        strcpy(item->release_id, p->release_id); strcpy(item->version, p->version);
        strcpy(item->name, p->name); strcpy(item->description, p->description);
        strcpy(item->presentation, p->presentation); strcpy(item->icon, p->icon);
        item->bytes = p->total_size; item->components = p->component_count;
        /* An updated NAND copy keeps the badge for the same application, even
         * while NOR still contains the older release. */
        item->boot = has_boot && !strcmp(boot.code, p->code) && !strcmp(boot.application_id, p->application_id);
        for (size_t i = 0; i < p->file_count; i++) {
            const ideas_file_t *file = &p->files[i];
            if (strcmp(file->name, p->icon) || file->size > 65536) continue;
            char path[IDEAS_PATH_BYTES];
            if (ideas_store_path(ROOT, p->code, p->icon, path) != ESP_OK ||
                verify_file(file, path, NULL) != ESP_OK) break;
            FILE *f = fopen(path, "rb");
            unsigned char header[24];
            /* Bounded PNG dimensions prevent a tiny encoded icon from asking
             * the asynchronous image decoder for a huge surface. */
            bool valid = f && fread(header, 1, sizeof(header), f) == sizeof(header) &&
                !memcmp(header, "\x89PNG\r\n\x1a\n", 8) && !memcmp(header + 12, "IHDR", 4) &&
                !header[16] && !header[17] && !header[18] && header[19] && header[19] <= 96 &&
                !header[20] && !header[21] && !header[22] && header[23] && header[23] <= 96;
            if (valid) {
                void *data = malloc(file->size);
                rewind(f);
                if (data && fread(data, 1, file->size, f) == file->size && !ferror(f))
                    icons[count-1] = (icon_t){data, file->size};
                else free(data);
            }
            if (f) fclose(f);
            break;
        }
    }
    if (dir) closedir(dir);
    LOCK();
    for (size_t i = 0; i < IDEAS_MAX_ITEMS; i++) { free(s_icons[i].data); s_icons[i] = icons[i]; }
    memcpy(s_state.items, items, IDEAS_MAX_ITEMS*sizeof(*items));
    s_state.count = count; s_state.invalid = invalid; s_state.scan_error = ESP_OK;
    ++s_state.generation; UNLOCK();
    free(items); free(p); return ESP_OK;
}
static esp_err_t http_open(const char *path, const char *body, esp_http_client_handle_t *out)
{
    const char *origin = CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL;
    const char *host = origin + (strlen(origin) >= 8 ? 8 : 0);
    if (strncmp(origin, "https://", 8) || !*host || strpbrk(host, "/?#@\r\n") ||
        !CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID[0]) return ESP_ERR_INVALID_STATE;
    char url[512];
    int n = snprintf(url, sizeof(url), "%s%s", origin, path);
    if (n < 0 || n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t config = {.url = url, .timeout_ms = 15000,
        .disable_auto_redirect = true, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = IO_BYTES, .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    if (body) esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, body ? strlen(body) : 0);
    if (err == ESP_OK && body) {
        size_t sent = 0, len = strlen(body);
        while (sent < len) {
            int written = esp_http_client_write(client, body + sent, len - sent);
            if (written <= 0) { err = ESP_FAIL; break; }
            sent += written;
        }
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200)
        err = esp_http_client_get_status_code(client) == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (err != ESP_OK) { esp_http_client_close(client); esp_http_client_cleanup(client); return err; }
    *out = client; return ESP_OK;
}
static esp_err_t resolve(const job_t *job, const ideas_package_t *local, char **json, size_t *size)
{
    factory_network_snapshot_t network;
    if (factory_network_get_snapshot(&network) != ESP_OK || network.state != FACTORY_NETWORK_CONNECTED)
        return ESP_ERR_INVALID_STATE;
    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "code", job->code);
    cJSON_AddStringToObject(body, "board_id", CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID);
    cJSON_AddStringToObject(body, "locale", CONFIG_IRIS_FACTORY_IDEAS_LOCALE);
    cJSON_AddNumberToObject(body, "recovery_abi", 2);
    cJSON_AddNumberToObject(body, "layout_version", 5);
    uint32_t flash_size = 0;
    esp_flash_get_size(esp_flash_default_chip, &flash_size);
    cJSON_AddNumberToObject(body, "flash_size", flash_size);
    if (local) {
        cJSON_AddStringToObject(body, "application_id", local->application_id);
        cJSON_AddStringToObject(body, "current_release_id", local->release_id);
    }
    char *encoded = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    if (!encoded) return ESP_ERR_NO_MEM;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = http_open("/api/v1/ideas/resolve", encoded, &client);
    cJSON_free(encoded);
    if (err != ESP_OK) return err;
    char *buffer = malloc(IDEAS_DESCRIPTOR_BYTES + 1); size_t used = 0;
    if (!buffer) err = ESP_ERR_NO_MEM;
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (err == ESP_OK && used <= IDEAS_DESCRIPTOR_BYTES) {
        int n = esp_http_client_read(client, buffer + used, IDEAS_DESCRIPTOR_BYTES + 1 - used);
        if (n < 0 || esp_timer_get_time() > deadline) { err = ESP_ERR_TIMEOUT; break; }
        if (!n) break;
        used += n;
        if (used > IDEAS_DESCRIPTOR_BYTES) { err = ESP_ERR_INVALID_SIZE; break; }
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_SIZE;
    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (err != ESP_OK) { free(buffer); return err; }
    buffer[used] = 0; *json = buffer; *size = used; return ESP_OK;
}
static esp_err_t fetch_file(const ideas_file_t *file, const char *path, void *ctx)
{
    (void)ctx;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = http_open(file->path, NULL, &client);
    if (err != ESP_OK) return err;
    FILE *f = fopen(path, "wb"); uint8_t *buffer = malloc(IO_BYTES);
    int64_t length = esp_http_client_get_content_length(client);
    if (!f || !buffer) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && length >= 0 && length != file->size) err = ESP_ERR_INVALID_SIZE;
    uint32_t used = 0;
    int64_t deadline = esp_timer_get_time() + 300000000;
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, (char *)buffer, IO_BYTES);
        if (n < 0 || esp_timer_get_time() > deadline) { err = ESP_ERR_TIMEOUT; break; }
        if (!n) break;
        if ((uint64_t)used + n > file->size) { err = ESP_ERR_INVALID_SIZE; break; }
        if (fwrite(buffer, 1, n, f) != n) { err = ESP_FAIL; break; }
        used += n;
        LOCK(); s_state.received += n; UNLOCK();
        vTaskDelay(1);
    }
    if (err == ESP_OK && (used != file->size || !esp_http_client_is_complete_data_received(client))) err = ESP_ERR_INVALID_SIZE;
    if (f) {
        if (fflush(f) || fsync(fileno(f))) err = ESP_FAIL;
        if (fclose(f)) err = ESP_FAIL;
    }
    free(buffer); esp_http_client_close(client); esp_http_client_cleanup(client);
    return err;
}
static esp_err_t run(const job_t *job)
{
    if (job->command == IDEAS_SCAN) return scan();
    ideas_package_t *local = calloc(1, sizeof(*local)), *remote = calloc(1, sizeof(*remote));
    if (!local || !remote) { free(local); free(remote); return ESP_ERR_NO_MEM; }
    esp_err_t existing = ideas_store_recover(ROOT, job->code);
    if (existing == ESP_OK) existing = ideas_store_read(ROOT, job->code, local);
    esp_err_t err = ESP_OK;
    char *json = NULL; size_t size = 0;
    if (job->command == IDEAS_PREVIEW) {
        err = existing;
        char path[IDEAS_PATH_BYTES];
        const ideas_file_t *file = NULL;
        for (size_t i = 0; err == ESP_OK && i < local->file_count; i++)
            if (!strcmp(local->files[i].name, local->presentation)) file = &local->files[i];
        if (err == ESP_OK && !file) err = ESP_ERR_NOT_FOUND;
        if (err == ESP_OK) err = ideas_store_path(ROOT, job->code, file->name, path);
        if (err == ESP_OK) err = verify_file(file, path, NULL);
        if (err == ESP_OK) err = ideas_presentation_show(factory_ui_handle(), path);
    } else if (job->command == IDEAS_INSTALL) {
        message("Setting boot application", "Verifying downloaded files");
        err = existing;
        if (err == ESP_OK) err = ideas_store_verify(ROOT, local, verify_file, NULL);
        char manifest_path[IDEAS_PATH_BYTES];
        if (err == ESP_OK) err = ideas_store_path(ROOT, job->code, "manifest.json", manifest_path);
        /* Save identity after all application/data readbacks, before the final
         * layout/boot commit. An early admission failure preserves the old
         * record, and power loss after commit cannot lose the new identity. */
        if (err == ESP_OK) err = factory_nand_install_idea(manifest_path, local->manifest, save_boot, local);
        if (err == ESP_OK) message("Boot application ready", "Starts on next power-on.\nRestart to run it now.");
        (void)scan();
    } else {
        message(job->command == IDEAS_CHECK ? "Checking for updates" : "Downloading Idea", "Finding published application");
        if (job->command == IDEAS_CHECK && existing != ESP_OK) err = existing;
        LOCK(); bool full = s_state.count >= IDEAS_MAX_ITEMS; UNLOCK();
        if (err == ESP_OK && existing != ESP_OK && full) err = ESP_ERR_NO_MEM;
        if (err == ESP_OK) err = resolve(job, existing == ESP_OK ? local : NULL, &json, &size);
        if (err == ESP_OK) err = ideas_package_parse(json, size, job->code, remote);
        uint32_t flash_size = 0; esp_flash_get_size(esp_flash_default_chip, &flash_size);
        if (err == ESP_OK && (remote->flash_size != flash_size ||
            (existing == ESP_OK && strcmp(local->application_id, remote->application_id)))) err = ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && job->command == IDEAS_CHECK) {
            bool available = strcmp(local->release_id, remote->release_id) != 0;
            LOCK();
            for (size_t i = 0; i < s_state.count; i++) if (!strcmp(s_state.items[i].code, job->code)) {
                s_state.items[i].update_available = available;
                strcpy(s_state.items[i].latest_version, remote->version);
            }
            ++s_state.generation; UNLOCK();
            char detail[160];
            snprintf(detail, sizeof(detail), "Latest version: %.64s\n%s", remote->version,
                     available ? "Return to My Ideas to download." : "The downloaded Idea is up to date.");
            message(available ? "Update available" : "Already up to date", detail);
        } else if (err == ESP_OK) {
            LOCK(); s_state.total = remote->total_size; UNLOCK();
            message("Downloading Idea", "Saving and verifying files in NAND");
            err = ideas_store_download(ROOT, json, size, remote, fetch_file, verify_file, NULL);
            if (err == ESP_OK) {
                (void)scan();
                message("Download complete", "Saved in My Ideas.\nSelect it to set as boot app.");
            }
        }
    }
    free(json); free(local); free(remote); return err;
}
static void worker(void *arg)
{
    job_t *job = arg;
    esp_err_t err = factory_nand_acquire();
    if (err == ESP_OK) { err = run(job); factory_nand_release(); }
    LOCK();
    s_state.busy = false; s_state.scanning = false;
    if (job->command == IDEAS_SCAN) s_state.scan_error = err;
    else if (job->command != IDEAS_PREVIEW || err != ESP_OK) {
        s_state.terminal = true; s_state.failed = err != ESP_OK; s_state.error = err;
        if (err != ESP_OK) {
            snprintf(s_state.title, sizeof(s_state.title), "%s failed", job->command == IDEAS_INSTALL ? "Installation" : job->command == IDEAS_CHECK ? "Update check" : "Download");
            snprintf(s_state.detail, sizeof(s_state.detail), "%s (0x%x).\nReturn to My Ideas to retry.", esp_err_to_name(err), (unsigned)err);
        }
        ++s_state.revision;
    }
    UNLOCK(); free(job); vTaskDelete(NULL);
}
esp_err_t factory_ideas_request(factory_ideas_command_t command, const char *code)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (command < IDEAS_SCAN || command > IDEAS_START ||
        (command != IDEAS_SCAN && !ideas_code_valid(code))) return ESP_ERR_INVALID_ARG;
    LOCK();
    if (s_state.busy) { UNLOCK(); return ESP_ERR_INVALID_STATE; }
    if (command == IDEAS_START) {
        bool boot = false;
        for (size_t i = 0; i < s_state.count; i++)
            boot |= s_state.items[i].boot && !strcmp(s_state.items[i].code, code);
        UNLOCK();
        if (!boot) return ESP_ERR_INVALID_STATE;
        esp_restart(); return ESP_OK;
    }
    s_state.busy = true; s_state.command = command; s_state.scanning = command == IDEAS_SCAN;
    if (command != IDEAS_SCAN && command != IDEAS_PREVIEW) {
        s_state.terminal = s_state.failed = false; s_state.total = s_state.received = 0;
        snprintf(s_state.title, sizeof(s_state.title), "%s", command == IDEAS_INSTALL ? "Setting boot application" : command == IDEAS_CHECK ? "Checking for updates" : "Downloading Idea");
        snprintf(s_state.detail, sizeof(s_state.detail), "Please wait");
        ++s_state.revision;
    }
    UNLOCK();
    job_t *job = calloc(1, sizeof(*job));
    if (job) { job->command = command; if (code) strcpy(job->code, code); }
    if (!job || xTaskCreate(worker, "ideas", 24576, job, 3, NULL) != pdPASS) {
        free(job); LOCK(); s_state.busy = s_state.scanning = false; UNLOCK(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t factory_ideas_snapshot(factory_ideas_snapshot_t *out)
{
    if (!out || !s_lock) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_state; UNLOCK(); return ESP_OK;
}
void *factory_ideas_copy_icon(const char *code, size_t *size)
{
    if (!code || !size || !s_lock) return NULL;
    *size = 0;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return NULL;
    void *copy = NULL;
    for (size_t i = 0; i < s_state.count; i++) if (!strcmp(code, s_state.items[i].code) && s_icons[i].data) {
        copy = malloc(s_icons[i].size);
        if (copy) { memcpy(copy, s_icons[i].data, s_icons[i].size); *size = s_icons[i].size; }
        break;
    }
    UNLOCK(); return copy;
}
esp_err_t factory_ideas_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? factory_ideas_request(IDEAS_SCAN, NULL) : ESP_ERR_NO_MEM;
}
