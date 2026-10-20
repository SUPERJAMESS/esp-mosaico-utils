#include "sdk.h"
#include SOURCE

static int fail_step, step;
static unsigned netifs, wifi, handlers, starts, stops, scans, connects, nvs_erases, update_requests;
static int32_t ip_subscription;
static bool saved_credentials, saved_connect_fails;
static bool fail_shared_commit;
static bool fail_wifi_stop;
static bool fail_saved_read, fail_station_config;
static bool fail_scan_results;
static unsigned scan_result_reads;
static bool driver_associated, change_during_ip_read;
static bool stop_during_scan_read;
static bool ip_during_disconnect, check_setup_busy;
static bool disconnect_during_setup, disconnect_during_config;
static bool fail_ap_probe, fail_ip_probe;
static wifi_ap_record_t driver_ap;
static esp_netif_ip_info_t driver_ip;
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
esp_err_t esp_wifi_stop(void) {
    ++stops;
    if (fail_wifi_stop) return ESP_FAIL;
    driver_associated = false; driver_ip.ip.addr = 0; return ESP_OK;
}
esp_err_t esp_wifi_deinit(void) { assert(wifi == 1); --wifi; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int mode) { assert(mode == WIFI_STORAGE_RAM); return next(); }
esp_err_t esp_wifi_set_mode(int mode) { return next(); }
esp_err_t esp_wifi_set_ps(int mode) { assert(mode == WIFI_PS_NONE); return next(); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id,
    mock_event_callback_t cb, void *ctx, esp_event_handler_instance_t *out)
{
    if (next()) return ESP_FAIL;
    if (b == IP_EVENT) ip_subscription = id;
    ++handlers; *out = (void *)(uintptr_t)b; return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id,
    esp_event_handler_instance_t handle)
{
    assert(handle == (void *)(uintptr_t)b && handlers); --handlers; return ESP_OK;
}
esp_err_t esp_wifi_set_config(int i, const wifi_config_t *c)
{
    if (disconnect_during_config) {
        disconnect_during_config = false;
        wifi_event_sta_disconnected_t event = {.reason = 1};
        unsigned before = connects;
        network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
        assert(connects == before); /* Configuration must finish before reconnecting. */
    }
    if (check_setup_busy) {
        check_setup_busy = false;
        assert(factory_network_forget() == ESP_ERR_INVALID_STATE);
        assert(factory_network_connect("overlap", "other-password") == ESP_ERR_INVALID_STATE);
        assert(factory_network_request_scan() == ESP_ERR_INVALID_STATE);
    }
    if (fail_station_config) return ESP_FAIL;
    configured = *c; return ESP_OK;
}
esp_err_t esp_wifi_connect(void) { ++connects; return saved_connect_fails ? ESP_FAIL : ESP_OK; }
esp_err_t esp_wifi_disconnect(void) {
    if (disconnect_during_setup) {
        disconnect_during_setup = false;
        wifi_event_sta_disconnected_t event = {.reason = 1};
        unsigned before = connects;
        network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
        assert(connects == before); /* Do not reconnect the AP being replaced. */
    }
    if (ip_during_disconnect) {
        ip_during_disconnect = false;
        ip_event_got_ip_t event = {.esp_netif = &netif, .ip_info = driver_ip};
        network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
        assert(!strcmp(shared.ssid, "saved-ap") && !strcmp(shared.password, "password"));
    }
    driver_associated = false; driver_ip.ip.addr = 0; return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {
    if (fail_ap_probe) return ESP_FAIL;
    if (!driver_associated) return ESP_ERR_WIFI_NOT_CONNECT;
    *out = driver_ap; return ESP_OK;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out) {
    if (fail_ip_probe) return ESP_FAIL;
    assert(n == &netif); *out = driver_ip;
    if (change_during_ip_read) {
        change_during_ip_read = false;
        assert(factory_network_connect("saved-ap", "changed-password") == ESP_OK);
    }
    return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const void *p, bool block) { assert(wifi == 1); ++scans; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n) {
    ++scan_result_reads;
    if (stop_during_scan_read) {
        stop_during_scan_read = false;
        assert(factory_network_suspend(true) == ESP_OK);
    }
    if (fail_scan_results) return ESP_FAIL;
    *n = 0; return ESP_OK;
}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *p) { return ESP_OK; }
void esp_ip4addr_ntoa(const esp_ip4_addr_t *ip, char *out, size_t size) { strlcpy(out, "192.0.2.1", size); }
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
    if (fail_saved_read) return ESP_FAIL;
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
    fail_wifi_stop = false;
    fail_saved_read = fail_station_config = false;
}
static void confirm_connection(ip_event_got_ip_t *event)
{
    driver_associated = true;
    memset(&driver_ap, 0, sizeof(driver_ap));
    memcpy(driver_ap.ssid, configured.sta.ssid, sizeof(configured.sta.ssid));
    driver_ip.ip.addr = 1;
    event->esp_netif = &netif; event->ip_info = driver_ip;
    network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, event);
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strncmp(argv[1], "transition-", 11)) {
        const char *scenario = argv[1];
        saved_credentials = true;
        assert(factory_network_start() == ESP_OK);
        ip_event_got_ip_t ip = {0};
        confirm_connection(&ip);
        assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED);
        char old_ssid[33], old_password[65];
        strcpy(old_ssid, shared.ssid); strcpy(old_password, shared.password);
        if (!strcmp(scenario, "transition-config-failure") || !strcmp(scenario, "transition-connect-failure")) {
            fail_station_config = !strcmp(scenario, "transition-config-failure");
            saved_connect_fails = !strcmp(scenario, "transition-connect-failure");
            assert(factory_network_connect("other-ap", "other-password") == ESP_FAIL);
            fail_station_config = saved_connect_fails = false;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(!strcmp(shared.ssid, old_ssid) && !strcmp(shared.password, old_password));
            assert(!s_network.pending_credentials && s_network.snapshot.state == FACTORY_NETWORK_FAILED);
            wifi_event_sta_disconnected_t disconnected = {.reason = 1};
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
            assert(s_network.snapshot.state == FACTORY_NETWORK_FAILED);
        } else if (!strcmp(scenario, "transition-stopped-retry")) {
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
            unsigned previous_starts = starts, previous_connects = connects;
            assert(factory_network_request_scan() == ESP_OK);
            assert(starts == previous_starts + 1 && connects == previous_connects + 1);
            assert(netifs == 1 && wifi == 1 && handlers == 2);
        } else if (!strcmp(scenario, "transition-late-scan") || !strcmp(scenario, "transition-scan-stop-during-read") ||
                   !strcmp(scenario, "transition-scan-stop-during-success")) {
            assert(factory_network_request_scan() == ESP_OK);
            bool during = strcmp(scenario, "transition-late-scan") != 0;
            if (during) stop_during_scan_read = true;
            else assert(factory_network_suspend(true) == ESP_OK);
            unsigned previous_reads = scan_result_reads;
            uint32_t previous_generation = s_network.snapshot.scan_generation;
            fail_scan_results = strcmp(scenario, "transition-scan-stop-during-success") != 0;
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
            assert(scan_result_reads == previous_reads + during);
            assert(s_network.snapshot.scan_generation == previous_generation);
            assert(s_network.snapshot.state == FACTORY_NETWORK_STOPPED && s_network.snapshot.last_error == ESP_OK);
        } else if (!strcmp(scenario, "transition-old-ap-ip") || !strcmp(scenario, "transition-same-ssid-ip")) {
            bool same = !strcmp(scenario, "transition-same-ssid-ip");
            assert(factory_network_connect(same ? "saved-ap" : "other-ap", "other-password") == ESP_OK);
            if (!same) driver_associated = true; /* Old AP remains the source of a queued IP event. */
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(!strcmp(shared.ssid, old_ssid) && !strcmp(shared.password, old_password));
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTING && !s_network.snapshot.ip[0]);
            confirm_connection(&ip);
            assert(!strcmp(shared.ssid, same ? "saved-ap" : "other-ap") && !strcmp(shared.password, "other-password"));
        } else if (!strcmp(scenario, "transition-request-during-ip-read")) {
            change_during_ip_read = true;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(!change_during_ip_read);
            assert(!strcmp(shared.password, old_password));
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTING && !s_network.snapshot.ip[0]);
            confirm_connection(&ip);
            assert(!strcmp(shared.password, "changed-password"));
        } else if (!strcmp(scenario, "transition-ap-probe-failure") || !strcmp(scenario, "transition-netif-probe-failure")) {
            assert(factory_network_connect("other-ap", "other-password") == ESP_OK);
            fail_ap_probe = !strcmp(scenario, "transition-ap-probe-failure");
            fail_ip_probe = !strcmp(scenario, "transition-netif-probe-failure");
            confirm_connection(&ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_FAILED && s_network.snapshot.last_error == ESP_FAIL);
            assert(!strcmp(shared.ssid, old_ssid));
            fail_ap_probe = fail_ip_probe = false;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && !strcmp(shared.ssid, "other-ap"));
        } else if (!strcmp(scenario, "transition-ip-identity")) {
            assert(factory_network_connect("other-ap", "other-password") == ESP_OK);
            driver_associated = true;
            memset(&driver_ap, 0, sizeof(driver_ap));
            memcpy(driver_ap.ssid, configured.sta.ssid, sizeof(configured.sta.ssid));
            driver_ip.ip.addr = 2;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(!strcmp(shared.ssid, old_ssid) && s_network.snapshot.state == FACTORY_NETWORK_CONNECTING);
            ip_event_got_ip_t foreign = ip; esp_netif_t other;
            foreign.esp_netif = &other; foreign.ip_info = driver_ip;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &foreign);
            assert(!strcmp(shared.ssid, old_ssid));
            ip.ip_info = driver_ip;
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && !strcmp(shared.ssid, "other-ap"));
        } else if (!strcmp(scenario, "transition-stop-pending")) {
            assert(factory_network_connect("unconfirmed-ap", "unconfirmed-password") == ESP_OK);
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
            assert(factory_network_request_scan() == ESP_OK);
            confirm_connection(&ip);
            assert(!strcmp(shared.ssid, old_ssid) && !strcmp(shared.password, old_password));
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED);
        } else if (!strcmp(scenario, "transition-ip-during-disconnect") || !strcmp(scenario, "transition-setup-busy") ||
                   !strcmp(scenario, "transition-disconnect-during-setup") || !strcmp(scenario, "transition-disconnect-during-config")) {
            ip_during_disconnect = !strcmp(scenario, "transition-ip-during-disconnect");
            check_setup_busy = !strcmp(scenario, "transition-setup-busy");
            disconnect_during_setup = !strcmp(scenario, "transition-disconnect-during-setup");
            disconnect_during_config = !strcmp(scenario, "transition-disconnect-during-config");
            assert(factory_network_connect("other-ap", "other-password") == ESP_OK);
            assert(!ip_during_disconnect && !check_setup_busy && !disconnect_during_setup && !disconnect_during_config);
            confirm_connection(&ip);
            assert(!strcmp(shared.ssid, "other-ap") && !strcmp(shared.password, "other-password"));
        } else assert(!"unknown transition scenario");
        reset(); puts(scenario); return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "status-", 7)) {
        const char *scenario = argv[1];
        saved_credentials = true;
        assert(factory_network_start() == ESP_OK);
        ip_event_got_ip_t ip = {0};
        confirm_connection(&ip);
        assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && s_network.snapshot.ip[0]);
        unsigned checks = update_requests;
        if (!strcmp(scenario, "status-ip-subscription")) {
            assert(ip_subscription == ESP_EVENT_ANY_ID);
        } else if (!strcmp(scenario, "status-lost-ip")) {
            mock_now += 60000000; /* A long-established connection loses DHCP. */
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTING && !s_network.snapshot.ip[0]);
            wifi_event_sta_disconnected_t disconnected = {.reason = 1};
            unsigned before = connects;
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
            assert(connects == before + 1 && s_network.snapshot.state == FACTORY_NETWORK_CONNECTING);
            confirm_connection(&ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && s_network.snapshot.ip[0]);
        } else if (!strcmp(scenario, "status-stopped")) {
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
            assert(s_network.snapshot.state == FACTORY_NETWORK_STOPPED && !s_network.snapshot.ip[0]);
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_STOPPED && update_requests == checks);
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
            confirm_connection(&ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && s_network.snapshot.ip[0]);
        } else if (!strcmp(scenario, "status-forgotten")) {
            assert(factory_network_forget() == ESP_OK);
            network_event(&s_network, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_NO_CREDENTIALS && !s_network.snapshot.ip[0]);
            assert(update_requests == checks && !shared.has_ssid && !shared.has_password);
            wifi_event_sta_disconnected_t disconnected = {.reason = 1};
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
            assert(s_network.snapshot.state == FACTORY_NETWORK_NO_CREDENTIALS);
        } else if (!strcmp(scenario, "status-reconnect")) {
            wifi_event_sta_disconnected_t disconnected = {.reason = 1};
            unsigned before = connects;
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
            assert(connects == before + 1 && s_network.snapshot.state == FACTORY_NETWORK_CONNECTING);
            assert(!s_network.snapshot.ip[0]);
            confirm_connection(&ip);
            assert(s_network.snapshot.state == FACTORY_NETWORK_CONNECTED && s_network.snapshot.ip[0]);
        } else if (!strcmp(scenario, "status-retry-failed")) {
            saved_connect_fails = true;
            wifi_event_sta_disconnected_t disconnected = {.reason = 1};
            network_event(&s_network, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
            assert(s_network.snapshot.state == FACTORY_NETWORK_FAILED && s_network.snapshot.last_error == ESP_FAIL);
        } else assert(!"unknown connectivity scenario");
        reset(); puts(scenario); return 0;
    }
    if (argc > 1) {
        assert(argc == 2);
        const char *scenario = argv[1];
        bool pending = !strcmp(scenario, "pending"), missing = !strcmp(scenario, "missing"),
             start_failure = !strcmp(scenario, "start-failure"), stop_failure = !strcmp(scenario, "stop-failure"),
             read_failure = !strcmp(scenario, "read-failure"), config_failure = !strcmp(scenario, "config-failure"),
             connect_failure = !strcmp(scenario, "connect-failure");
        bool resume_failure = read_failure || config_failure || connect_failure;
        assert(pending || missing || start_failure || stop_failure || resume_failure);
        saved_credentials = pending || resume_failure;
        assert(factory_network_start() == ESP_OK);
        if (pending) assert(factory_network_connect("unconfirmed-ap", "new-password") == ESP_OK);
        if (stop_failure) {
            unsigned previous_stops = stops;
            fail_wifi_stop = true;
            assert(factory_network_suspend(true) == ESP_FAIL);
            fail_wifi_stop = false;
            assert(factory_network_suspend(true) == ESP_OK && stops == previous_stops + 2);
        } else assert(factory_network_suspend(true) == ESP_OK);
        assert(factory_network_connect("late-ap", "password") == ESP_ERR_INVALID_STATE);
        assert(factory_network_request_scan() == ESP_ERR_INVALID_STATE);
        if (start_failure) {
            fail_step = step + 1;
            assert(factory_network_suspend(false) == ESP_FAIL);
            fail_step = 0;
        }
        if (resume_failure) {
            fail_saved_read = read_failure;
            fail_station_config = config_failure;
            saved_connect_fails = connect_failure;
            assert(factory_network_suspend(false) == ESP_FAIL);
            assert(s_network.snapshot.state == FACTORY_NETWORK_FAILED);
            fail_saved_read = fail_station_config = saved_connect_fails = false;
        }
        unsigned previous_starts = starts, previous_connects = connects;
        assert(factory_network_suspend(false) == ESP_OK && starts == previous_starts + !resume_failure);
        if (resume_failure) assert(connects == previous_connects + 1);
        factory_network_snapshot_t snapshot;
        assert(factory_network_get_snapshot(&snapshot) == ESP_OK);
        if (pending || resume_failure) {
            assert(!memcmp(configured.sta.ssid, "saved-ap", 8));
            assert(snapshot.state == FACTORY_NETWORK_CONNECTING && snapshot.last_error == ESP_OK);
            ip_event_got_ip_t ip = {0};
            confirm_connection(&ip);
            fprintf(stderr, "resumed SSID=%s; persisted SSID=%s\n", snapshot.ssid, shared.ssid);
            assert(!strcmp(shared.ssid, "saved-ap") && !strcmp(shared.password, "password"));
        } else {
            fprintf(stderr, "resumed state=%u; suspended=%u\n", snapshot.state, s_network.suspended);
            assert(snapshot.state == FACTORY_NETWORK_NO_CREDENTIALS && !snapshot.credentials_saved);
            assert(!snapshot.ssid[0] && !snapshot.ip[0] && snapshot.last_error == ESP_OK);
        }
        reset(); puts(scenario); return 0;
    }
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
    confirm_connection(&event);
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
    confirm_connection(&ip);
    assert(shared.has_password && !shared.password[0] && !strcmp(shared.ssid, "open-ap"));
    char ssid[33], password[65];
    memset(ssid, 's', 32); ssid[32] = 0;
    memset(password, 'a', 64); password[64] = 0;
    assert(factory_network_connect(ssid, password) == ESP_OK);
    assert(!memcmp(configured.sta.ssid, ssid, 32) && !memcmp(configured.sta.password, password, 64));
    confirm_connection(&ip);
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
    return 0;
}
