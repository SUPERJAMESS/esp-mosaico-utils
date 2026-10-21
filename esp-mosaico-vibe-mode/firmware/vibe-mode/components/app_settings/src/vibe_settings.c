// SPDX-License-Identifier: Apache-2.0
#include "vibe_settings.h"
#include "factory_locale.h"
#include "factory_network.h"
#include "factory_network_credentials.h"
#include "factory_system_metadata.h"
#include "mosaico_boot.h"
#include "factory_system_update.h"
#include "app_storage.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EXT_RAM_BSS_ATTR vibe_settings_snapshot_t s_settings;
static SemaphoreHandle_t s_lock;
static unsigned s_wifi_revision;
typedef struct {
    vibe_settings_command_t command;
    char key[VIBE_SHARED_KEY_BYTES], value[VIBE_SHARED_VALUE_BYTES];
} request_t;

unsigned vibe_settings_language(void)
{
    if (!s_lock) return 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned language = s_settings.language;
    xSemaphoreGive(s_lock);
    return language == 2 ? 2 : 1;
}
bool vibe_shared_key_valid(const char *key)
{
    return key && *key && strlen(key) < VIBE_SHARED_KEY_BYTES &&
           strspn(key, "abcdefghijklmnopqrstuvwxyz0123456789_") == strlen(key);
}
bool vibe_shared_key_secret(const char *key)
{
    if (!key) return false;
    char lower[64];
    size_t i = 0;
    for (; key[i] && i + 1 < sizeof(lower); ++i)
        lower[i] = (key[i] >= 'A' && key[i] <= 'Z') ? (char)(key[i] - 'A' + 'a') : key[i];
    lower[i] = 0;
    return strstr(lower, "key") || strstr(lower, "token") || strstr(lower, "secret") ||
           strstr(lower, "pass") || strstr(lower, "pwd");
}
static esp_err_t open_namespace(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, name, mode, handle);
}
esp_err_t vibe_shared_get(const char *key, char *value, size_t size)
{
    if (!vibe_shared_key_valid(key) || !value || !size)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = open_namespace("shared", NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_get_str(handle, key, value, &size);
    nvs_close(handle);
    return err;
}
static esp_err_t reset_marker(bool set)
{
    nvs_handle_t handle;
    esp_err_t err = open_namespace("vibe_reset", NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = set ? nvs_set_u8(handle, "pending", 1) : nvs_erase_key(handle, "pending");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
static esp_err_t clear_preferences(void)
{
    /* Explicit allowlist: there is intentionally no partition erase, filesystem
     * traversal, format, boot-image write or identity reset in this operation. */
    static const char *const names[] = {"wifi", "shared", "vibe_prefs", "settings"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        nvs_handle_t handle;
        esp_err_t err = open_namespace(names[i], NVS_READWRITE, &handle);
        if (err != ESP_OK)
            return err;
        err = nvs_erase_all(handle);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
        if (err != ESP_OK)
            return err;
    }
    /* A deferred remote launch must not bypass the first-run setup after
     * reset. Keep all other bridge state and the current Idea boot record. */
    nvs_handle_t h;
    esp_err_t err = open_namespace("iris_bridge_v2", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, "pending_boot");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    return ESP_OK;
}
static unsigned read_number(nvs_handle_t handle, const char *key, unsigned fallback)
{
    uint32_t n;
    return nvs_get_u32(handle, key, &n) == ESP_OK ? n : fallback;
}
static void load(vibe_settings_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->brightness = 80;
    out->screen_off_seconds = 0;
    out->auto_rotate = out->haptics = out->open_confirm = true;
    nvs_handle_t h;
    if (open_namespace("vibe_prefs", NVS_READONLY, &h) == ESP_OK) {
        out->language = read_number(h, "lang", 0);
        if (out->language > 2) out->language = 0;
        out->brightness = read_number(h, "brightness", 80);
        if (out->brightness < 20 || out->brightness > 100) out->brightness = 80;
        out->screen_off_seconds = read_number(h, "timeout", 0);
        if (out->screen_off_seconds > 600) out->screen_off_seconds = 0;
        out->auto_rotate = read_number(h, "rotate", 1) != 0;
        out->haptics = read_number(h, "haptics", 1) != 0;
        out->open_confirm_count = read_number(h, "confirm_cnt", 0);
        if (out->open_confirm_count > 3) out->open_confirm_count = 3;
        out->open_confirm = read_number(h, "open_confirm", 1) != 0 && out->open_confirm_count < 3;
        nvs_close(h);
    }
    /* Adopt main's earlier language preference until the user saves a
     * product preference. Reset clears both namespaces. */
    if (!out->language && open_namespace("settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t language;
        if (nvs_get_u8(h, "language", &language) == ESP_OK && language <= 1)
            out->language = language ? 1 : 2;
        nvs_close(h);
    }
    if (!out->language) out->language = 1;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(FACTORY_SYSTEM_METADATA_PARTITION, "shared", NVS_TYPE_STR, &it);
    while (err == ESP_OK && out->shared_count < VIBE_SHARED_MAX) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        vibe_shared_entry_t *entry = &out->shared[out->shared_count];
        if (vibe_shared_key_valid(info.key) &&
            vibe_shared_get(info.key, entry->value, sizeof(entry->value)) == ESP_OK) {
            snprintf(entry->key, sizeof(entry->key), "%s", info.key);
            ++out->shared_count;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}
static esp_err_t apply(const request_t *request)
{
    if (request->command == VIBE_SETTINGS_RESET) {
        /* Serialize with NOR writers and NAND transactions before changing
         * preferences or requesting a reboot. The services remain usable if
         * admission fails; no reset marker has been persisted yet. */
        /* Reserve only: a damaged/unmounted NAND must not prevent a settings
         * reset, and reset must never mount or modify its filesystem. */
        esp_err_t err = app_storage_reserve();
        if (err != ESP_OK) return err;
        err = factory_system_update_read_acquire();
        if (err != ESP_OK) { app_storage_release(); return err; }
        err = mosaico_boot_request_recovery();
        if (err == ESP_OK) err = reset_marker(true);
        /* Stop reconnect/credential persistence before clearing the allowlist. */
        if (err == ESP_OK) err = factory_network_suspend(true);
        if (err == ESP_OK) err = factory_network_forget();
        if (err == ESP_OK) err = clear_preferences();
        if (err == ESP_OK) esp_restart();
        factory_system_update_read_release();
        app_storage_release();
        if (err != ESP_OK) (void)factory_network_suspend(false);
        return err; /* Marker remains until next startup, also on partial failure. */
    }
    nvs_handle_t h;
    bool shared = request->command >= VIBE_SETTINGS_SHARED_SET;
    esp_err_t err = open_namespace(shared ? "shared" : "vibe_prefs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (shared) {
        err = request->command == VIBE_SETTINGS_SHARED_DELETE
                  ? nvs_erase_key(h, request->key) : nvs_set_str(h, request->key, request->value);
        if (err == ESP_ERR_NVS_NOT_FOUND && request->command == VIBE_SETTINGS_SHARED_DELETE)
            err = ESP_OK;
    } else if (request->command == VIBE_SETTINGS_OPEN_CONFIRM) {
        uint32_t mode = (uint32_t)strtoul(request->value, NULL, 10);
        if (mode == 2) {
            unsigned cnt = read_number(h, "confirm_cnt", 0);
            if (cnt < 3) ++cnt;
            err = nvs_set_u32(h, "confirm_cnt", cnt);
            if (err == ESP_OK && cnt >= 3) err = nvs_set_u32(h, "open_confirm", 0);
        } else {
            err = nvs_set_u32(h, "open_confirm", mode ? 1 : 0);
            if (err == ESP_OK) err = nvs_set_u32(h, "confirm_cnt", mode ? 0 : 3);
        }
    } else {
        static const char *const keys[] = {"lang", "brightness", "timeout", "rotate", "haptics", "open_confirm"};
        err = nvs_set_u32(h, keys[request->command], (uint32_t)strtoul(request->value, NULL, 10));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
static void worker(void *arg)
{
    request_t *request = arg;
    esp_err_t err = request ? apply(request) : ESP_OK;
    if (request) memset(request, 0, sizeof(*request));
    free(request);
    unsigned wifi_revision = factory_network_credentials_revision();
    vibe_settings_snapshot_t *next = calloc(1, sizeof(*next));
    if (next) load(next);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t revision = s_settings.revision + 1;
    if (next) { s_settings = *next; s_wifi_revision = wifi_revision; }
    s_settings.busy = false;
    s_settings.error = err != ESP_OK ? err : next ? ESP_OK : ESP_ERR_NO_MEM;
    s_settings.revision = revision;
    xSemaphoreGive(s_lock);
    factory_locale_use(vibe_settings_language() == 1);
    if (next) { memset(next, 0, sizeof(*next)); free(next); }
    vTaskDelete(NULL);
}
esp_err_t vibe_settings_request(vibe_settings_command_t command, const char *key, const char *value)
{
    if (!s_lock || command < VIBE_SETTINGS_LANGUAGE || command > VIBE_SETTINGS_RESET)
        return ESP_ERR_INVALID_ARG;
    if (command == VIBE_SETTINGS_SHARED_SET || command == VIBE_SETTINGS_SHARED_DELETE) {
        if (!vibe_shared_key_valid(key) || (command == VIBE_SETTINGS_SHARED_SET &&
            (!value || strlen(value) >= VIBE_SHARED_VALUE_BYTES))) return ESP_ERR_INVALID_ARG;
        if (command == VIBE_SETTINGS_SHARED_SET &&
            ((!strcmp(key, "wifi_ssid") && !factory_network_ssid_valid(value)) ||
             (!strcmp(key, "wifi_password") && !factory_network_password_valid(value))))
            return ESP_ERR_INVALID_ARG;
    } else if (command != VIBE_SETTINGS_RESET) {
        if (!value || !*value || strlen(value) > 3 || strspn(value, "0123456789") != strlen(value))
            return ESP_ERR_INVALID_ARG;
        unsigned n = (unsigned)strtoul(value, NULL, 10);
        if ((command == VIBE_SETTINGS_LANGUAGE && (n < 1 || n > 2)) ||
            (command == VIBE_SETTINGS_BRIGHTNESS && (n < 20 || n > 100 || n % 20)) ||
            (command == VIBE_SETTINGS_TIMEOUT && n != 0 && n != 60 && n != 180 && n != 300 && n != 600) ||
            (command == VIBE_SETTINGS_OPEN_CONFIRM ? n > 2 : command >= VIBE_SETTINGS_ROTATE && n > 1)) return ESP_ERR_INVALID_ARG;
    }
    request_t *request = calloc(1, sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    request->command = command;
    if (key) snprintf(request->key, sizeof(request->key), "%s", key);
    if (value) snprintf(request->value, sizeof(request->value), "%s", value);
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) { free(request); return ESP_ERR_TIMEOUT; }
    unsigned user_entries = 0;
    for (size_t i = 0; i < s_settings.shared_count; ++i)
        if (strcmp(s_settings.shared[i].key, "wifi_ssid") &&
            strcmp(s_settings.shared[i].key, "wifi_password")) ++user_entries;
    bool full = command == VIBE_SETTINGS_SHARED_SET && user_entries >= VIBE_SHARED_USER_MAX &&
                strcmp(key, "wifi_ssid") && strcmp(key, "wifi_password");
    for (size_t i = 0; full && i < s_settings.shared_count; ++i)
        if (!strcmp(key, s_settings.shared[i].key)) full = false;
    if (s_settings.busy || full) {
        xSemaphoreGive(s_lock); memset(request, 0, sizeof(*request)); free(request);
        return full ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    s_settings.busy = true;
    BaseType_t started = xTaskCreate(worker, "vibe_settings", 4096, request, 3, NULL);
    if (started != pdPASS) s_settings.busy = false;
    xSemaphoreGive(s_lock);
    if (started != pdPASS) { memset(request, 0, sizeof(*request)); free(request); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
esp_err_t vibe_settings_snapshot(vibe_settings_snapshot_t *out)
{
    if (!out || !s_lock) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_settings.busy && s_wifi_revision != factory_network_credentials_revision()) {
        /* NVS enumeration stays off the render task. Wi-Fi saves must become
         * visible in Shared information without requiring a reboot. */
        s_settings.busy = true;
        if (xTaskCreate(worker, "vibe_settings", 4096, NULL, 3, NULL) != pdPASS)
            s_settings.busy = false;
    }
    *out = s_settings;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
esp_err_t vibe_settings_init(void)
{
    if (s_lock) return ESP_OK;
    nvs_handle_t h;
    uint8_t pending = 0;
    esp_err_t err = open_namespace("vibe_reset", NVS_READONLY, &h);
    if (err == ESP_OK) { err = nvs_get_u8(h, "pending", &pending); nvs_close(h); }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (pending) {
        err = clear_preferences();
        if (err == ESP_OK) err = reset_marker(false);
        if (err != ESP_OK) return err;
    }
    s_wifi_revision = factory_network_credentials_revision();
    load(&s_settings);
    /* Persist the default (or the adopted preference) once. Both early locale
     * readers and the settings UI use the same key on subsequent boots. */
    err = open_namespace("vibe_prefs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint32_t saved = 0;
    err = nvs_get_u32(h, "lang", &saved);
    if (err == ESP_ERR_NVS_NOT_FOUND || (err == ESP_OK && (saved < 1 || saved > 2))) {
        err = nvs_set_u32(h, "lang", s_settings.language);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) return err;
    s_settings.revision = 1;
    s_lock = xSemaphoreCreateMutex();
    factory_locale_use(vibe_settings_language() == 1);
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}
