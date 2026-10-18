#include "sdk.h"
#include SOURCE

static int fail_step, step;
static unsigned netifs, wifi, handlers, starts, scans, connects, nvs_erases, update_requests;
static bool saved_credentials, saved_connect_fails;
static bool fail_shared_commit;
static struct { char ssid[33], password[65]; bool has_ssid, has_password; } shared, staged;
static wifi_config_t configured;
static esp_netif_t netif;
void factory_update_check_request(bool manual) { (void)manual; ++update_requests; }
static esp_err_t next(void) { return ++step == fail_step ? ESP_FAIL : ESP_OK; }
esp_err_t esp_netif_init(void) { return next(); }
esp_err_t esp_event_loop_create_default(void)
{
    return next() == ESP_OK ? ESP_ERR_INVALID_STATE : ESP_FAIL; /* Shared loop already exists. */
}
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    if (next()) return NULL;
    assert(netifs == 0); ++netifs; return &netif;
}
void esp_netif_destroy_default_wifi(esp_netif_t *p) { assert(p == &netif && netifs == 1); --netifs; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c)
{
    assert(factory_network_start() == ESP_ERR_INVALID_STATE); /* Concurrent start cannot enter. */
    if (next()) return ESP_FAIL;
    assert(!wifi); ++wifi; return ESP_OK;
}
esp_err_t esp_wifi_start(void) { if (next()) return ESP_FAIL; ++starts; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { assert(wifi == 1); --wifi; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int mode) { assert(mode == WIFI_STORAGE_RAM); return next(); }
esp_err_t esp_wifi_set_mode(int mode) { return next(); }
esp_err_t esp_wifi_set_ps(int mode) { assert(mode == WIFI_PS_NONE); return next(); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id,
    mock_event_callback_t cb, void *ctx, esp_event_handler_instance_t *out)
{
    if (next()) return ESP_FAIL;
    ++handlers; *out = (void *)(uintptr_t)b; return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id,
    esp_event_handler_instance_t handle)
{
    assert(handle == (void *)(uintptr_t)b && handlers); --handlers; return ESP_OK;
}
esp_err_t esp_wifi_set_config(int i, const wifi_config_t *c) { configured = *c; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++connects; return saved_connect_fails ? ESP_FAIL : ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t esp_wifi_scan_start(const void *p, bool block) { assert(wifi == 1); ++scans; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n) { *n = 0; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *p) { return ESP_OK; }
void esp_ip4addr_ntoa(const int *ip, char *out, size_t size) { strlcpy(out, "192.0.2.1", size); }
esp_err_t esp_read_mac(uint8_t *mac, int type) { memset(mac, 0, 6); return ESP_OK; }
esp_err_t esp_iris_format_device_id(char *out) { memset(out, '0', 32); out[32] = 0; return ESP_OK; }
esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_hostname_set(const char *v) { return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *v) { return ESP_OK; }
esp_err_t mdns_service_add(const char *a, const char *b, const char *c, unsigned p,
    const mdns_txt_item_t *txt, size_t n) { return ESP_OK; }
void mdns_free(void) { }
esp_err_t nvs_open_from_partition(const char *part, const char *ns, int mode, nvs_handle_t *h)
{
    assert(!strcmp(part, "sysmeta"));
    assert(!strcmp(ns, "wifi") || !strcmp(ns, "shared"));
    *h = !strcmp(ns, "wifi") ? 1 : 2;
    if (*h == 2) { if (mode == NVS_READWRITE) staged = shared; return ESP_OK; }
    return saved_credentials || mode == NVS_READWRITE ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{
    const char *value;
    if (h == 1) {
        if (!saved_credentials) return ESP_ERR_NVS_NOT_FOUND;
        value = !strcmp(key, "ssid") ? "saved-ap" : "password";
    } else {
        bool ssid = !strcmp(key, "wifi_ssid");
        if (ssid ? !shared.has_ssid : !shared.has_password) return ESP_ERR_NVS_NOT_FOUND;
        value = ssid ? shared.ssid : shared.password;
    }
    assert(strlen(value) + 1 <= *size);
    strlcpy(out, value, *size); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    assert(h == 2);
    if (!strcmp(k, "wifi_ssid")) { strcpy(staged.ssid, v); staged.has_ssid = true; }
    else { assert(!strcmp(k, "wifi_password")); strcpy(staged.password, v); staged.has_password = true; }
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) { assert(h == 1); saved_credentials = false; ++nvs_erases; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    assert(h == 2); /* No other shared key may be erased. */
    if (!strcmp(k, "wifi_ssid")) staged.has_ssid = false;
    else { assert(!strcmp(k, "wifi_password")); staged.has_password = false; }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    if (h == 2) { if (fail_shared_commit) return ESP_FAIL; shared = staged; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { }

static void reset(void)
{
    cleanup_failed_start(&s_network);
    if (s_network.lock) vSemaphoreDelete(s_network.lock);
    memset(&s_network, 0, sizeof(s_network));
    step = fail_step = 0; saved_credentials = saved_connect_fails = false;
    assert(!netifs && !wifi && !handlers && !mock_allocations);
    memset(&shared, 0, sizeof(shared)); fail_shared_commit = false; nvs_erases = 0;
}
int main(void)
{
    mock_alloc_fail_after = 0;
    assert(factory_network_start() == ESP_ERR_NO_MEM);
    mock_alloc_fail_after = -1;
    assert(factory_network_start() == ESP_OK);
    assert(scans == 0); /* Startup does not launch AP discovery. */
    assert(factory_network_request_scan() == ESP_OK && scans == 1);
    reset();
    for (int failure = 1; failure <= 10; ++failure) {
        fail_step = failure;
        assert(factory_network_start() != ESP_OK);
        assert(!netifs && !wifi && !handlers);
        factory_network_snapshot_t snapshot;
        assert(factory_network_get_snapshot(&snapshot) == ESP_OK);
        assert(!snapshot.started && snapshot.state == FACTORY_NETWORK_FAILED && snapshot.last_error);
        SemaphoreHandle_t retained_lock = s_network.lock;
        step = fail_step = 0;
        /* Exercise the actual UI/USB entry points, not only startup itself. */
        if (failure % 2)
            assert(factory_network_request_scan() == ESP_OK);
        else
            assert(factory_network_connect("new-ap", "password") == ESP_OK);
        assert(s_network.lock == retained_lock);
        assert(netifs == 1 && wifi == 1 && handlers == 2);
        assert(factory_network_get_snapshot(&snapshot) == ESP_OK && snapshot.started);
        unsigned previous_starts = starts;
        assert(factory_network_start() == ESP_OK && starts == previous_starts);
        reset();
    }
    saved_credentials = saved_connect_fails = true;
    unsigned scans_before = scans;
    assert(factory_network_start() == ESP_OK);
    assert(scans == scans_before);
    assert(s_network.snapshot.started && s_network.snapshot.state == FACTORY_NETWORK_FAILED);
    saved_connect_fails = false;
    assert(factory_network_connect("replacement-ap", "password") == ESP_OK);
    assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTING);
    assert(connects && scans);
    ip_event_got_ip_t event = {0};
    network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
    assert(update_requests == 1 && s_network.snapshot.state == FACTORY_NETWORK_CONNECTED);
    reset();
    /* Existing private credentials migrate, reconnect and become shared. */
    saved_credentials = true;
    assert(factory_network_start() == ESP_OK);
    assert(!saved_credentials && shared.has_ssid && shared.has_password);
    assert(!strcmp(shared.ssid, "saved-ap") && !strcmp(shared.password, "password"));
    assert(factory_network_credentials_revision() && !memcmp(configured.sta.ssid, "saved-ap", 8));
    /* A confirmed new connection replaces the shared values, including open APs. */
    assert(factory_network_connect("open-ap", "") == ESP_OK);
    ip_event_got_ip_t ip = {0};
    network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(shared.has_password && !shared.password[0] && !strcmp(shared.ssid, "open-ap"));
    char ssid[33], password[65];
    memset(ssid, 's', 32); ssid[32] = 0;
    memset(password, 'a', 64); password[64] = 0;
    assert(factory_network_connect(ssid, password) == ESP_OK);
    assert(!memcmp(configured.sta.ssid, ssid, 32) && !memcmp(configured.sta.password, password, 64));
    network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(!strcmp(shared.password, password));
    cleanup_failed_start(&s_network); vSemaphoreDelete(s_network.lock);
    memset(&s_network, 0, sizeof(s_network)); step = 0;
    assert(factory_network_start() == ESP_OK && !memcmp(configured.sta.password, password, 64));
    assert(factory_network_forget() == ESP_OK && !shared.has_ssid && !shared.has_password);
    ssid[0] = password[0] = 0;
    assert(credentials_load(ssid, password) == ESP_ERR_NVS_NOT_FOUND);
    reset();
    /* Migration failure retains the old credentials for a later retry. */
    saved_credentials = fail_shared_commit = true;
    assert(factory_network_start() == ESP_OK && s_network.snapshot.last_error == ESP_FAIL);
    assert(saved_credentials && !shared.has_ssid && !nvs_erases);
    reset();
    puts("Wi-Fi partial startup rollback, provisioning retries and credentials passed");
}
