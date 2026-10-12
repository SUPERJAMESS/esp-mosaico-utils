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
#include "esp_heap_caps.h"
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
typedef struct {
    ideas_package_t package;
    char *json;
    size_t json_size, card_size;
    void *card;
    char summary[160];
    bool already_downloaded;
} pending_t;
static pending_t *s_pending;
static uint32_t s_request_id;
typedef struct {
    factory_ideas_command_t command;
    char code[7];
    uint32_t request_id;
    pending_t *pending;
    bool up_to_date;
} job_t;
static bool cancelled(uint32_t request_id)
{
    LOCK(); bool value = request_id != s_state.preview_id; UNLOCK(); return value;
}
static void free_pending(pending_t *p)
{
    if (!p) return;
    free(p->json); heap_caps_free(p->card); free(p);
}
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
        if (cancelled(job->request_id)) { err = ESP_ERR_INVALID_STATE; break; }
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
    const pending_t *pending = ctx;
    if (pending && pending->card && !strcmp(file->name, pending->package.presentation)) {
        FILE *cached = fopen(path, "wb");
        if (!cached) return ESP_FAIL;
        bool ok = fwrite(pending->card, 1, pending->card_size, cached) == pending->card_size;
        if (fflush(cached) || fsync(fileno(cached))) ok = false;
        if (fclose(cached)) ok = false;
        if (ok) { LOCK(); s_state.received += pending->card_size; UNLOCK(); }
        return ok ? ESP_OK : ESP_FAIL;
    }
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
static bool ascii_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 32 || *p > 126) return false;
    return true;
}
static esp_err_t fetch_card(const job_t *job, pending_t *pending)
{
    const ideas_package_t *p = &pending->package;
    if (!p->presentation[0])
        return ascii_text(p->name) && ascii_text(p->description) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    const ideas_file_t *file = NULL;
    for (size_t i = 0; i < p->file_count; ++i)
        if (!strcmp(p->files[i].name, p->presentation)) file = &p->files[i];
    if (!file || file->size > IDEAS_MAX_ASSET_BYTES) return ESP_ERR_INVALID_SIZE;
    pending->card = heap_caps_aligned_alloc(64, file->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pending->card) return ESP_ERR_NO_MEM;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = http_open(file->path, NULL, &client);
    if (err != ESP_OK) return err;
    int64_t length = esp_http_client_get_content_length(client);
    if (length >= 0 && length != file->size) err = ESP_ERR_INVALID_SIZE;
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (err == ESP_OK && used < file->size) {
        if (cancelled(job->request_id)) { err = ESP_ERR_INVALID_STATE; break; }
        size_t nmax = file->size - used;
        if (nmax > IO_BYTES) nmax = IO_BYTES;
        int n = esp_http_client_read(client, (char *)pending->card + used, nmax);
        if (n < 0 || esp_timer_get_time() > deadline) { err = ESP_ERR_TIMEOUT; break; }
        if (!n) break;
        used += n; vTaskDelay(1);
    }
    if (err == ESP_OK && (used != file->size || !esp_http_client_is_complete_data_received(client)))
        err = ESP_ERR_INVALID_SIZE;
    esp_http_client_close(client); esp_http_client_cleanup(client);
    uint8_t digest[32]; size_t size = 0; char actual[65];
    if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, pending->card, used, digest, sizeof(digest), &size) != PSA_SUCCESS || size != 32)) err = ESP_FAIL;
    if (err == ESP_OK) { hex(digest, actual); if (strcmp(actual, file->sha256)) err = ESP_ERR_INVALID_CRC; }
    if (err == ESP_OK) pending->card_size = used;
    return err;
}
static void preview_item(factory_idea_t *item, const ideas_package_t *p)
{
    memset(item, 0, sizeof(*item));
    strcpy(item->code, p->code); strcpy(item->application_id, p->application_id);
    strcpy(item->release_id, p->release_id); strcpy(item->version, p->version);
    strcpy(item->name, p->name); strcpy(item->description, p->description);
    item->bytes = p->total_size; item->components = p->component_count;
}
static esp_err_t prepare_download(job_t *job)
{
    pending_t *pending = calloc(1, sizeof(*pending));
    ideas_package_t *local = calloc(1, sizeof(*local));
    if (!pending || !local) { free(pending); free(local); return ESP_ERR_NO_MEM; }
    job->pending = pending;
    /* Only reading the local descriptor requires a NAND lease. Network IO,
     * the preview and the wait for user confirmation never retain it. */
    esp_err_t err = factory_nand_acquire(), existing = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        existing = ideas_store_read(ROOT, job->code, local);
        factory_nand_release();
    }
    if (err == ESP_OK && job->command == IDEAS_CHECK && existing != ESP_OK) err = existing;
    LOCK(); bool full = s_state.count >= IDEAS_MAX_ITEMS; UNLOCK();
    if (err == ESP_OK && full && existing != ESP_OK) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && cancelled(job->request_id)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = resolve(job, existing == ESP_OK ? local : NULL, &pending->json, &pending->json_size);
    ideas_package_t *remote = &pending->package;
    if (err == ESP_OK) err = ideas_package_parse(pending->json, pending->json_size, job->code, remote);
    uint32_t flash_size = 0; esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (err == ESP_OK && (remote->flash_size != flash_size ||
        (existing == ESP_OK && strcmp(local->application_id, remote->application_id)))) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && existing == ESP_OK && !strcmp(local->release_id, remote->release_id)) {
        err = factory_nand_acquire();
        if (err == ESP_OK) {
            pending->already_downloaded = ideas_store_verify(ROOT, remote, verify_file, NULL) == ESP_OK;
            factory_nand_release();
        }
    }
    /* Local verification may outlive Cancel; do not start another HTTP request. */
    if (err == ESP_OK && cancelled(job->request_id)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && job->command == IDEAS_CHECK && pending->already_downloaded) {
        job->up_to_date = true;
        message("Already up to date", "The downloaded Idea is up to date.");
    } else if (err == ESP_OK) {
        err = fetch_card(job, pending);
        if (err == ESP_OK && cancelled(job->request_id)) err = ESP_ERR_INVALID_STATE;
        unsigned long kib = (unsigned long)((remote->total_size + 1023) / 1024);
        snprintf(pending->summary, sizeof(pending->summary), "%s%.64s | %lu KiB",
            pending->already_downloaded ? "Downloaded: " : "Version: ", remote->version, kib);
        if (err == ESP_OK && pending->card) {
            const ideas_presentation_options_t options = {.request_id = job->request_id,
                .summary = pending->summary, .already_downloaded = pending->already_downloaded,
                .decide = factory_ideas_confirm};
            err = ideas_presentation_open(factory_ui_handle(), pending->card, pending->card_size, &options);
        }
    }
    free(local); return err;
}
static esp_err_t run_local(const job_t *job)
{
    if (job->command == IDEAS_SCAN) return scan();
    if (job->command == IDEAS_DOWNLOAD) {
        pending_t *p = job->pending;
        message("Downloading Idea", "Saving and verifying files in NAND");
        esp_err_t err = ideas_store_download(ROOT, p->json, p->json_size, &p->package, fetch_file, verify_file, p);
        if (err == ESP_OK) { (void)scan(); message("Download complete", "Saved in My Ideas.\nSelect it to set as boot app."); }
        return err;
    }
    ideas_package_t *local = calloc(1, sizeof(*local));
    if (!local) return ESP_ERR_NO_MEM;
    esp_err_t err = ideas_store_recover(ROOT, job->code);
    if (err == ESP_OK) err = ideas_store_read(ROOT, job->code, local);
    if (job->command == IDEAS_PREVIEW) {
        char path[IDEAS_PATH_BYTES]; const ideas_file_t *file = NULL;
        for (size_t i = 0; err == ESP_OK && i < local->file_count; i++)
            if (!strcmp(local->files[i].name, local->presentation)) file = &local->files[i];
        if (err == ESP_OK && !file) err = ESP_ERR_NOT_FOUND;
        if (err == ESP_OK) err = ideas_store_path(ROOT, job->code, file->name, path);
        if (err == ESP_OK) err = verify_file(file, path, NULL);
        if (err == ESP_OK) err = ideas_presentation_show(factory_ui_handle(), path);
    } else if (job->command == IDEAS_INSTALL) {
        message("Setting boot application", "Verifying downloaded files");
        if (err == ESP_OK) err = ideas_store_verify(ROOT, local, verify_file, NULL);
        char path[IDEAS_PATH_BYTES];
        if (err == ESP_OK) err = ideas_store_path(ROOT, job->code, "manifest.json", path);
        if (err == ESP_OK) err = factory_nand_install_idea(path, local->manifest, save_boot, local);
        if (err == ESP_OK) message("Boot application ready", "Starts on next power-on.\nRestart to run it now.");
        (void)scan();
    }
    free(local); return err;
}
static void worker(void *arg)
{
    job_t *job = arg;
    bool preparing = job->command == IDEAS_RESOLVE || job->command == IDEAS_CHECK;
    esp_err_t err = ESP_OK;
    if (preparing) err = prepare_download(job);
    else {
        if (job->pending) ideas_presentation_close();
        if (job->command != IDEAS_DISMISS) {
            err = factory_nand_acquire();
            if (err == ESP_OK) { err = run_local(job); factory_nand_release(); }
        }
    }
    LOCK();
    bool stale = preparing && job->request_id != s_state.preview_id;
    if (preparing && (err != ESP_OK || stale || job->up_to_date)) {
        UNLOCK(); ideas_presentation_close(); LOCK();
        stale = job->request_id != s_state.preview_id;
    }
    if (preparing && !stale && err == ESP_OK) {
        for (size_t i = 0; i < s_state.count; ++i) if (!strcmp(s_state.items[i].code, job->code)) {
            factory_idea_t *item = &s_state.items[i];
            item->update_available = strcmp(item->release_id, job->pending->package.release_id) != 0;
            snprintf(item->latest_version, sizeof(item->latest_version), "%s", job->pending->package.version);
            ++s_state.generation;
            break;
        }
    }
    bool ready = preparing && !stale && err == ESP_OK && !job->up_to_date;
    if (ready) {
        s_pending = job->pending; job->pending = NULL;
        preview_item(&s_state.preview, &s_pending->package);
        s_state.already_downloaded = s_pending->already_downloaded;
        s_state.preview_ready = true;
        ideas_presentation_enable();
    }
    s_state.busy = false; s_state.scanning = false;
    if (preparing) s_state.resolving = false;
    if (job->command == IDEAS_SCAN) s_state.scan_error = err;
    else if (!stale && !ready && job->command != IDEAS_DISMISS &&
             (job->command != IDEAS_PREVIEW || err != ESP_OK)) {
        s_state.terminal = true; s_state.failed = err != ESP_OK; s_state.error = err;
        if (preparing) s_state.preview_id = 0;
        if (err != ESP_OK) {
            snprintf(s_state.title, sizeof(s_state.title), "%s failed", preparing ? "App information" :
                job->command == IDEAS_INSTALL ? "Installation" : "Download");
            snprintf(s_state.detail, sizeof(s_state.detail), "%s (0x%x).\nReturn home and try again.", esp_err_to_name(err), (unsigned)err);
        }
        ++s_state.revision;
    }
    UNLOCK();
    free_pending(job->pending); free(job); vTaskDelete(NULL);
}
static esp_err_t launch_locked(job_t *job)
{
    if (xTaskCreate(worker, "ideas", 24576, job, 3, NULL) == pdPASS) return ESP_OK;
    return ESP_ERR_NO_MEM;
}
esp_err_t factory_ideas_confirm(uint32_t request_id, bool download)
{
    if (!s_lock || !request_id) return ESP_ERR_INVALID_STATE;
    job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    LOCK();
    if (s_state.busy || !s_state.preview_ready || !s_pending || request_id != s_state.preview_id) {
        UNLOCK(); free(job); return ESP_ERR_INVALID_STATE;
    }
    job->command = download && !s_pending->already_downloaded ? IDEAS_DOWNLOAD : IDEAS_DISMISS;
    job->pending = s_pending; strcpy(job->code, s_pending->package.code);
    s_state.busy = true; s_state.preview_ready = false;
    s_state.command = job->command; s_state.terminal = s_state.failed = false;
    s_state.return_library = download && s_pending->already_downloaded;
    s_state.total = s_pending->package.total_size; s_state.received = 0;
    s_pending = NULL;
    esp_err_t err = launch_locked(job);
    if (err != ESP_OK) { s_pending = job->pending; s_state.preview_ready = true; s_state.busy = false; }
    else { s_state.preview_id = 0; ++s_state.revision; }
    UNLOCK();
    if (err != ESP_OK) free(job);
    return err;
}
esp_err_t factory_ideas_cancel(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    LOCK();
    uint32_t ready = s_state.preview_ready ? s_state.preview_id : 0;
    if (!ready && s_state.resolving) {
        s_state.preview_id = 0; s_state.resolving = false; s_state.terminal = false;
    }
    UNLOCK();
    return ready ? factory_ideas_confirm(ready, false) : ESP_OK;
}
esp_err_t factory_ideas_request(factory_ideas_command_t command, const char *code)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (command < IDEAS_SCAN || command > IDEAS_RESOLVE || command == IDEAS_DOWNLOAD ||
        (command != IDEAS_SCAN && !ideas_code_valid(code))) return ESP_ERR_INVALID_ARG;
    job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    LOCK();
    if (s_state.busy || s_pending) { UNLOCK(); free(job); return ESP_ERR_INVALID_STATE; }
    if (command == IDEAS_START) {
        bool boot = false;
        for (size_t i = 0; i < s_state.count; i++) boot |= s_state.items[i].boot && !strcmp(s_state.items[i].code, code);
        UNLOCK(); free(job);
        if (!boot) return ESP_ERR_INVALID_STATE;
        esp_restart(); return ESP_OK;
    }
    job->command = command; if (code) strcpy(job->code, code);
    s_state.busy = true; s_state.command = command; s_state.scanning = command == IDEAS_SCAN;
    if (command == IDEAS_RESOLVE || command == IDEAS_CHECK) {
        if (++s_request_id == 0) ++s_request_id;
        s_state.preview_id = job->request_id = s_request_id;
        s_state.resolving = true; s_state.preview_ready = false; s_state.return_library = false;
        s_state.already_downloaded = false; memset(&s_state.preview, 0, sizeof(s_state.preview));
    }
    if (command != IDEAS_SCAN && command != IDEAS_PREVIEW) {
        s_state.terminal = s_state.failed = false; s_state.total = s_state.received = 0;
        snprintf(s_state.title, sizeof(s_state.title), "%s", command == IDEAS_INSTALL ? "Setting boot application" : "Loading app information");
        snprintf(s_state.detail, sizeof(s_state.detail), "Please wait"); ++s_state.revision;
    }
    esp_err_t err = launch_locked(job);
    if (err != ESP_OK) { s_state.busy = s_state.scanning = s_state.resolving = false; s_state.preview_id = 0; }
    UNLOCK();
    if (err != ESP_OK) free(job);
    return err;
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
