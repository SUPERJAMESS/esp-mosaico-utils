// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "mosaico_shared.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include SOURCE
#include LOCALE_SOURCE

typedef struct { char key[16], value[257]; bool string; uint32_t number; } entry_t;
typedef struct { char name[16]; entry_t values[32], staged[32]; } namespace_t;
static namespace_t namespaces[12];
static unsigned count, commits, fail_commit, restarts;
static bool recovery, suspended, read_lease, storage_lease, writer_busy;
static unsigned wifi_revision;
unsigned factory_network_credentials_revision(void) { return wifi_revision; }
static void (*task)(void *);
static void *task_arg;
static entry_t *entry(nvs_handle_t h, const char *key, bool staged, bool create)
{
    entry_t *list = staged ? namespaces[h - 1].staged : namespaces[h - 1].values;
    for (unsigned i = 0; i < 32; ++i) if (!strcmp(list[i].key, key)) return &list[i];
    if (create) for (unsigned i = 0; i < 32; ++i) if (!list[i].key[0]) {
        snprintf(list[i].key, sizeof(list[i].key), "%s", key); return &list[i];
    }
    return NULL;
}
esp_err_t nvs_flash_init_partition(const char *p) { assert(!strcmp(p, "sysmeta")); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *p, const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(!strcmp(p, "sysmeta")); unsigned i;
    for (i = 0; i < count && strcmp(namespaces[i].name, ns); ++i) {}
    if (i == count) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        assert(count < 12); ++count; snprintf(namespaces[i].name, sizeof(namespaces[i].name), "%s", ns);
    }
    *out = i + 1;
    if (mode == NVS_READWRITE) memcpy(namespaces[i].staged, namespaces[i].values, sizeof(namespaces[i].values));
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *n)
{
    entry_t *e = entry(h, k, false, false); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    assert(e->string); size_t need = strlen(e->value) + 1;
    if (*n < need) { *n = need; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, e->value, need); *n = need; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ entry_t *e = entry(h, k, true, true); assert(e); e->string = true; strcpy(e->value, v); return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{ entry_t *e = entry(h, k, false, false); if (!e) return ESP_ERR_NVS_NOT_FOUND; *v = e->number; return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{ entry_t *e = entry(h, k, true, true); assert(e); e->number = v; e->string = false; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{ uint32_t n; esp_err_t err = nvs_get_u32(h, k, &n); if (!err) *v = n; return err; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return nvs_set_u32(h, k, v); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{ entry_t *e = entry(h, k, true, false); if (!e) return ESP_ERR_NVS_NOT_FOUND; memset(e, 0, sizeof(*e)); return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    const char *ns = namespaces[h - 1].name;
    assert(!strcmp(ns, "wifi") || !strcmp(ns, "shared") || !strcmp(ns, "vibe_prefs") || !strcmp(ns, "settings"));
    memset(namespaces[h - 1].staged, 0, sizeof(namespaces[h - 1].staged)); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    if (++commits == fail_commit) return ESP_FAIL;
    memcpy(namespaces[h - 1].values, namespaces[h - 1].staged, sizeof(namespaces[h - 1].values)); return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_entry_find(const char *p, const char *ns, int type, nvs_iterator_t *it)
{
    assert(!strcmp(p, "sysmeta") && !strcmp(ns, "shared") && type == NVS_TYPE_STR);
    *it = calloc(1, sizeof(unsigned)); assert(*it); **it = UINT32_MAX; return nvs_entry_next(it);
}
esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    unsigned n = **it + 1;
    for (; n < count * 32; ++n) if (!strcmp(namespaces[n / 32].name, "shared") &&
        namespaces[n / 32].values[n % 32].key[0]) { **it = n; return ESP_OK; }
    free(*it); *it = NULL; return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *out) { strcpy(out->key, namespaces[*it / 32].values[*it % 32].key); }
void nvs_release_iterator(nvs_iterator_t it) { free(it); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(bool)); }
int xSemaphoreTake(SemaphoreHandle_t m, uint32_t timeout) { if (*m) { assert(!timeout); return 0; } *m = true; return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t m) { assert(*m); *m = false; }
BaseType_t xTaskCreate(void (*f)(void *), const char *n, unsigned stack, void *arg, unsigned priority, void *out)
{ (void)n; (void)stack; (void)priority; (void)out; assert(!task); task = f; task_arg = arg; return pdPASS; }
void vTaskDelete(void *t) { assert(!t); }
esp_err_t factory_network_suspend(bool s) { suspended = s; return ESP_OK; }
esp_err_t factory_network_forget(void) { assert(suspended); return ESP_OK; }
esp_err_t mosaico_boot_request_recovery(void) { recovery = true; return ESP_OK; }
esp_err_t factory_system_update_read_acquire(void) { if (writer_busy) return ESP_ERR_INVALID_STATE; assert(!read_lease); read_lease = true; return ESP_OK; }
void factory_system_update_read_release(void) { assert(read_lease); read_lease = false; }
esp_err_t app_storage_reserve(void) { assert(!storage_lease); storage_lease = true; return ESP_OK; }
void app_storage_release(void) { assert(storage_lease); storage_lease = false; }
void esp_restart(void) { assert(recovery && suspended && read_lease && storage_lease); ++restarts; }
static void execute(vibe_settings_command_t c, const char *k, const char *v)
{
    assert(vibe_settings_request(c, k, v) == ESP_OK);
    assert(vibe_settings_request(c, k, v) == ESP_ERR_INVALID_STATE);
    void (*f)(void *) = task; task = NULL; f(task_arg);
    assert(!s_settings.busy && !read_lease && !storage_lease);
}
static void reboot(void)
{
    free(s_lock); s_lock = NULL; memset(&s_settings, 0, sizeof(s_settings));
    factory_locale_init();
    assert(vibe_settings_init() == ESP_OK);
}
int main(void)
{
    factory_locale_init(); assert(factory_locale_get() == 1);
    fail_commit = 1;
    assert(vibe_settings_init() == ESP_FAIL && !s_lock);
    fail_commit = 0;
    assert(vibe_settings_init() == ESP_OK && s_settings.language == 1);
    unsigned before = commits;
    reboot(); assert(s_settings.language == 1 && factory_locale_get() == 1 && commits == before);
    /* Emulate an existing device before the product preference was written. */
    free(s_lock); s_lock = NULL; memset(namespaces, 0, sizeof(namespaces)); count = 0;
    nvs_handle_t legacy;
    assert(!nvs_open_from_partition("sysmeta", "settings", NVS_READWRITE, &legacy));
    assert(!nvs_set_u8(legacy, "language", 0) && !nvs_commit(legacy));
    reboot(); assert(s_settings.language == 2 && factory_locale_get() == 0);
    execute(VIBE_SETTINGS_LANGUAGE, NULL, "1");
    reboot(); assert(s_settings.language == 1 && factory_locale_get() == 1);
    /* A failed language commit cannot change the displayed or next-boot locale. */
    fail_commit = commits + 1;
    execute(VIBE_SETTINGS_LANGUAGE, NULL, "2");
    assert(s_settings.error == ESP_FAIL && s_settings.language == 1 && factory_locale_get() == 1);
    fail_commit = 0; reboot(); assert(s_settings.language == 1);
    execute(VIBE_SETTINGS_LANGUAGE, NULL, "2");
    execute(VIBE_SETTINGS_BRIGHTNESS, NULL, "40");
    execute(VIBE_SETTINGS_SHARED_SET, "api_key", "never-log-this");
    char value[257];
    assert(mosaico_shared_get("api_key", value, sizeof(value)) == ESP_OK && !strcmp(value, "never-log-this"));
    assert(mosaico_shared_get("api_key", value, 2) == ESP_ERR_NVS_INVALID_LENGTH);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "Upper", "x") == ESP_ERR_INVALID_ARG);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "abcdefghijklmnop", "x") == ESP_ERR_INVALID_ARG);
    assert(vibe_settings_request(VIBE_SETTINGS_TIMEOUT, NULL, "120") == ESP_ERR_INVALID_ARG);
    reboot(); assert(s_settings.language == 2 && s_settings.brightness == 40 && s_settings.shared_count == 1);
    for (unsigned i = 1; i < 16; ++i) { char key[16]; snprintf(key, sizeof(key), "k%u", i); execute(VIBE_SETTINGS_SHARED_SET, key, "v"); }
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "overflow", "v") == ESP_ERR_NO_MEM);
    execute(VIBE_SETTINGS_SHARED_SET, "api_key", "replacement");
    execute(VIBE_SETTINGS_SHARED_DELETE, "k1", NULL);
    assert(s_settings.shared_count == 15);
    const char *preserved[] = {"iris", "identity", "ideas"};
    for (unsigned i = 0; i < 3; ++i) { nvs_handle_t h; assert(!nvs_open_from_partition("sysmeta", preserved[i], NVS_READWRITE, &h)); assert(!nvs_set_str(h, "sentinel", "keep")); assert(!nvs_commit(h)); }
    writer_busy = true; execute(VIBE_SETTINGS_RESET, NULL, NULL);
    assert(s_settings.error == ESP_ERR_INVALID_STATE && !recovery && !restarts && s_settings.shared_count == 15);
    writer_busy = false;
    /* Interrupt each clear after the durable marker. Boot must finish the
     * allowlist, retain identity/boot metadata, and never touch NAND/NOR. */
    for (unsigned cut = 2; cut <= 6; ++cut) {
        execute(VIBE_SETTINGS_SHARED_SET, "api_key", "secret");
        execute(VIBE_SETTINGS_LANGUAGE, NULL, "2");
        nvs_handle_t bridge;
        assert(!nvs_open_from_partition("sysmeta", "iris_bridge_v2", NVS_READWRITE, &bridge));
        assert(!nvs_set_str(bridge, "pending_boot", "deferred"));
        assert(!nvs_set_str(bridge, "sentinel", "keep"));
        assert(!nvs_commit(bridge));
        fail_commit = commits + cut;
        execute(VIBE_SETTINGS_RESET, NULL, NULL);
        assert(s_settings.error == ESP_FAIL && !restarts);
        fail_commit = 0; reboot();
        assert(s_settings.language == 1 && !s_settings.shared_count && s_settings.brightness == 80);
        size_t size = sizeof(value);
        assert(nvs_get_str(bridge, "pending_boot", value, &size) == ESP_ERR_NVS_NOT_FOUND);
        assert(!nvs_get_str(bridge, "sentinel", value, &size) && !strcmp(value, "keep"));
        for (unsigned i = 0; i < 3; ++i) { nvs_handle_t h; size_t n = sizeof(value); assert(!nvs_open_from_partition("sysmeta", preserved[i], NVS_READONLY, &h)); assert(!nvs_get_str(h, "sentinel", value, &n)); assert(!strcmp(value, "keep")); }
    }
    execute(VIBE_SETTINGS_RESET, NULL, NULL); assert(restarts == 1); reboot();
    assert(s_settings.language == 1 && !s_settings.shared_count);
    /* An asynchronous Wi-Fi save refreshes the shared page and public API. */
    nvs_handle_t shared;
    assert(!nvs_open_from_partition("sysmeta", "shared", NVS_READWRITE, &shared));
    assert(!nvs_set_str(shared, "wifi_ssid", "test-network"));
    assert(!nvs_set_str(shared, "wifi_password", "test-password"));
    assert(!nvs_commit(shared)); ++wifi_revision;
    vibe_settings_snapshot_t snapshot;
    assert(vibe_settings_snapshot(&snapshot) == ESP_OK && snapshot.busy && task);
    void (*refresh)(void *) = task; task = NULL; refresh(task_arg);
    assert(vibe_settings_snapshot(&snapshot) == ESP_OK && !snapshot.busy && snapshot.shared_count == 2);
    assert(!mosaico_shared_get("wifi_password", value, sizeof(value)) && !strcmp(value, "test-password"));
    assert(vibe_shared_key_secret("wifi_password"));
    for (unsigned i = 0; i < VIBE_SHARED_USER_MAX; ++i) {
        char key[16]; snprintf(key, sizeof(key), "custom%u", i);
        execute(VIBE_SETTINGS_SHARED_SET, key, "keep");
    }
    assert(s_settings.shared_count == VIBE_SHARED_USER_MAX + 2);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "overflow", "v") == ESP_ERR_NO_MEM);
    execute(VIBE_SETTINGS_SHARED_SET, "wifi_password", "new-password");
    assert(!mosaico_shared_get("wifi_password", value, sizeof(value)) && !strcmp(value, "new-password"));
    /* Reserved shared keys obey the same bounds as ordinary provisioning;
     * invalid edits must leave the last usable credentials intact. */
    char ssid[34], password[66];
    memset(ssid, 's', 33); ssid[33] = 0;
    memset(password, 'a', 65); password[65] = 0;
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "wifi_ssid", ssid) == ESP_ERR_INVALID_ARG);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "wifi_ssid", "") == ESP_ERR_INVALID_ARG);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "wifi_password", password) == ESP_ERR_INVALID_ARG);
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "wifi_password", "short") == ESP_ERR_INVALID_ARG);
    password[64] = 0; password[0] = 'z';
    assert(vibe_settings_request(VIBE_SETTINGS_SHARED_SET, "wifi_password", password) == ESP_ERR_INVALID_ARG);
    assert(!mosaico_shared_get("wifi_password", value, sizeof(value)) && !strcmp(value, "new-password"));
    ssid[32] = 0; password[0] = 'a';
    execute(VIBE_SETTINGS_SHARED_SET, "wifi_ssid", ssid);
    execute(VIBE_SETTINGS_SHARED_SET, "wifi_password", password);
    char loaded_ssid[33], loaded_password[65];
    assert(!mosaico_shared_get("wifi_ssid", loaded_ssid, sizeof(loaded_ssid)) && strlen(loaded_ssid) == 32);
    assert(!mosaico_shared_get("wifi_password", loaded_password, sizeof(loaded_password)) && strlen(loaded_password) == 64);
    execute(VIBE_SETTINGS_SHARED_SET, "wifi_password", ""); /* Open network. */
    memset(value, 'v', sizeof(value) - 1); value[sizeof(value) - 1] = 0;
    execute(VIBE_SETTINGS_SHARED_SET, "custom0", value); /* Ordinary values retain 256 bytes. */
    puts("preferences, shared application API and interrupted reset passed");
    return 0;
}
