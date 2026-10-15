#include "factory_network.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "factory_system_metadata.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "nvs.h"

#define FACTORY_WIFI_NAMESPACE "wifi"
#define FACTORY_WIFI_SSID_KEY "ssid"
#define FACTORY_WIFI_PASSWORD_KEY "password"
#define SHARED_NAMESPACE "shared"
#define SHARED_WIFI_SSID "wifi_ssid"
#define SHARED_WIFI_PASSWORD "wifi_password"
static atomic_uint s_credentials_revision;

typedef struct {
    SemaphoreHandle_t lock;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_events;
    esp_event_handler_instance_t ip_events;
    factory_network_snapshot_t snapshot;
    char pending_ssid[FACTORY_NETWORK_SSID_BYTES];
    char pending_password[FACTORY_NETWORK_PASSWORD_BYTES];
    int64_t connect_started_us;
    bool pending_credentials;
    bool mdns_started;
    bool wifi_initialized;
    bool suspended;
    atomic_flag starting;
} factory_network_context_t;

static const char *TAG = "factory_network";
static factory_network_context_t s_network = {.starting = ATOMIC_FLAG_INIT};

static esp_err_t request_scan_started(factory_network_context_t *ctx);

static void snapshot_set_error(factory_network_context_t *ctx, esp_err_t error,
                               factory_network_state_t state)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->snapshot.last_error = error;
    ctx->snapshot.state = state;
    xSemaphoreGive(ctx->lock);
}

static void scan_set_error(factory_network_context_t *ctx, esp_err_t error)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->snapshot.scanning = false;
    ctx->snapshot.last_error = error;
    xSemaphoreGive(ctx->lock);
}

static esp_err_t credentials_save(const char *ssid, const char *password);
static esp_err_t credentials_read(const char *space, const char *ssid_key, const char *password_key,
                                  char ssid[FACTORY_NETWORK_SSID_BYTES],
                                  char password[FACTORY_NETWORK_PASSWORD_BYTES])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION,
                                            space, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t ssid_size = FACTORY_NETWORK_SSID_BYTES;
    size_t password_size = FACTORY_NETWORK_PASSWORD_BYTES;
    err = nvs_get_str(handle, ssid_key, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, password_key, password, &password_size);
    }
    nvs_close(handle);
    return err;
}
static esp_err_t credentials_load(char ssid[FACTORY_NETWORK_SSID_BYTES],
                                  char password[FACTORY_NETWORK_PASSWORD_BYTES])
{
    esp_err_t err = credentials_read(SHARED_NAMESPACE, SHARED_WIFI_SSID, SHARED_WIFI_PASSWORD,
                                     ssid, password);
    if (err != ESP_ERR_NVS_NOT_FOUND || ssid[0]) return err;
    /* Adopt existing devices once, before the shared namespace becomes the
     * source of truth. Never replace an explicitly edited shared SSID. */
    err = credentials_read(FACTORY_WIFI_NAMESPACE, FACTORY_WIFI_SSID_KEY,
                           FACTORY_WIFI_PASSWORD_KEY, ssid, password);
    return err == ESP_OK ? credentials_save(ssid, password) : err;
}

static esp_err_t legacy_credentials_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION,
                                            FACTORY_WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION,
                                                SHARED_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open system metadata Wi-Fi namespace");
    esp_err_t err = nvs_set_str(handle, SHARED_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, SHARED_WIFI_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        atomic_fetch_add(&s_credentials_revision, 1);
        /* Only remove the former private copy after shared values commit. */
        err = legacy_credentials_erase();
    }
    return err;
}

static esp_err_t credentials_erase(void)
{
    /* Clear the old copy first so a partially interrupted forget cannot
     * re-import a network the user removed from the shared namespace. */
    ESP_RETURN_ON_ERROR(legacy_credentials_erase(), TAG, "clear former Wi-Fi settings");
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION,
                                            SHARED_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open factory Wi-Fi NVS");
    err = nvs_erase_key(handle, SHARED_WIFI_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(handle, SHARED_WIFI_PASSWORD);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) atomic_fetch_add(&s_credentials_revision, 1);
    return err;
}
unsigned factory_network_credentials_revision(void)
{
    return atomic_load(&s_credentials_revision);
}

static esp_err_t apply_station_config(factory_network_context_t *ctx, const char *ssid,
                                      const char *password)
{
    wifi_config_t config = {0};
    /* IDF uses fixed 32/64-byte arrays: full-length SSIDs/hex PSKs do not
     * require a trailing NUL and must not lose their final byte. */
    size_t ssid_size = strnlen(ssid, FACTORY_NETWORK_SSID_BYTES);
    size_t password_size = strnlen(password, FACTORY_NETWORK_PASSWORD_BYTES);
    ESP_RETURN_ON_FALSE(ssid_size && ssid_size <= sizeof(config.sta.ssid) &&
                            password_size <= sizeof(config.sta.password),
                        ESP_ERR_INVALID_SIZE, TAG, "invalid saved Wi-Fi lengths");
    memcpy(config.sta.ssid, ssid, ssid_size);
    memcpy(config.sta.password, password, password_size);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "set station configuration");

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    strlcpy(ctx->snapshot.ssid, ssid, sizeof(ctx->snapshot.ssid));
    ctx->snapshot.state = FACTORY_NETWORK_CONNECTING;
    ctx->snapshot.last_error = ESP_OK;
    ctx->snapshot.ip[0] = '\0';
    ctx->connect_started_us = esp_timer_get_time();
    xSemaphoreGive(ctx->lock);
    return esp_wifi_connect();
}

static void mdns_start(factory_network_context_t *ctx)
{
    if (ctx->mdns_started) {
        return;
    }

    char device_id[33] = {0};
    char suffix[7] = {0};
    if (esp_iris_format_device_id(device_id) == ESP_OK) {
        memcpy(suffix, device_id + sizeof(device_id) - sizeof(suffix), sizeof(suffix) - 1);
    } else {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
            return;
        }
        snprintf(suffix, sizeof(suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    }

    char hostname[FACTORY_NETWORK_HOSTNAME_BYTES];
    char instance[64];
    snprintf(hostname, sizeof(hostname), "%s-%s", CONFIG_IRIS_FACTORY_MDNS_PREFIX, suffix);
    snprintf(instance, sizeof(instance), "ESP-Mosaico %s", suffix);

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(hostname);
    }
    if (err == ESP_OK) {
        err = mdns_instance_name_set(instance);
    }
    char port[8];
    snprintf(port, sizeof(port), "%u", CONFIG_ESP_IRIS_TCP_PORT);
    mdns_txt_item_t txt[] = {
        {.key = "device_id", .value = device_id}, {.key = "mode", .value = "recovery"},
        {.key = "protocol", .value = "1"},        {.key = "pairing", .value = "hmac"},
        {.key = "port", .value = port},
    };
    if (err == ESP_OK) {
        err = mdns_service_add(instance, "_esp-iris", "_tcp", CONFIG_ESP_IRIS_TCP_PORT, txt,
                               sizeof(txt) / sizeof(txt[0]));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS setup failed: %s", esp_err_to_name(err));
        mdns_free();
        return;
    }

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    strlcpy(ctx->snapshot.hostname, hostname, sizeof(ctx->snapshot.hostname));
    ctx->mdns_started = true;
    xSemaphoreGive(ctx->lock);
    ESP_LOGI(TAG, "mDNS service %s.local:%u", hostname, CONFIG_ESP_IRIS_TCP_PORT);
}

static void scan_results_update(factory_network_context_t *ctx)
{
    uint16_t available = 0;
    if (esp_wifi_scan_get_ap_num(&available) != ESP_OK) {
        scan_set_error(ctx, ESP_FAIL);
        return;
    }
    uint16_t count = available;
    if (count > CONFIG_IRIS_FACTORY_WIFI_MAX_NETWORKS) {
        count = CONFIG_IRIS_FACTORY_WIFI_MAX_NETWORKS;
    }
    if (count > FACTORY_NETWORK_MAX_RESULTS) {
        count = FACTORY_NETWORK_MAX_RESULTS;
    }
    wifi_ap_record_t records[FACTORY_NETWORK_MAX_RESULTS] = {0};
    esp_err_t err = count > 0 ? esp_wifi_scan_get_ap_records(&count, records) : ESP_OK;
    if (err != ESP_OK) {
        scan_set_error(ctx, err);
        return;
    }

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->snapshot.ap_count = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < ctx->snapshot.ap_count; ++j) {
            if (strncmp(ctx->snapshot.aps[j].ssid, (const char *)records[i].ssid,
                        FACTORY_NETWORK_SSID_BYTES) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        factory_network_ap_t *ap = &ctx->snapshot.aps[ctx->snapshot.ap_count++];
        strlcpy(ap->ssid, (const char *)records[i].ssid, sizeof(ap->ssid));
        ap->rssi = records[i].rssi;
        ap->authmode = (uint8_t)records[i].authmode;
    }
    ctx->snapshot.scanning = false;
    ctx->snapshot.scan_generation++;
    xSemaphoreGive(ctx->lock);
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    factory_network_context_t *ctx = arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_results_update(ctx);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        const int64_t now = esp_timer_get_time();
        bool retry = false;
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        if (ctx->snapshot.state == FACTORY_NETWORK_CONNECTED) {
            ctx->connect_started_us = now;
        }
        const int64_t elapsed_ms = (now - ctx->connect_started_us) / 1000;
        ctx->snapshot.ip[0] = '\0';
        ctx->snapshot.last_error =
            event != NULL ? (esp_err_t)(ESP_ERR_WIFI_BASE + event->reason) : ESP_FAIL;
        retry = !ctx->suspended && (ctx->snapshot.credentials_saved || ctx->pending_credentials);
        if (retry && elapsed_ms < CONFIG_IRIS_FACTORY_WIFI_CONNECT_TIMEOUT_MS) {
            ctx->snapshot.state = FACTORY_NETWORK_CONNECTING;
        } else {
            ctx->snapshot.state = ctx->suspended ? FACTORY_NETWORK_STOPPED : FACTORY_NETWORK_FAILED;
            retry = false;
        }
        xSemaphoreGive(ctx->lock);
        if (retry) {
            (void)esp_wifi_connect();
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char ip[FACTORY_NETWORK_IP_BYTES] = {0};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));

        bool save = false;
        char ssid[FACTORY_NETWORK_SSID_BYTES] = {0};
        char password[FACTORY_NETWORK_PASSWORD_BYTES] = {0};
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        if (ctx->suspended) {
            xSemaphoreGive(ctx->lock);
            return;
        }
        strlcpy(ctx->snapshot.ip, ip, sizeof(ctx->snapshot.ip));
        ctx->snapshot.state = FACTORY_NETWORK_CONNECTED;
        ctx->snapshot.last_error = ESP_OK;
        if (ctx->pending_credentials) {
            strlcpy(ssid, ctx->pending_ssid, sizeof(ssid));
            strlcpy(password, ctx->pending_password, sizeof(password));
            ctx->pending_credentials = false;
            save = true;
        }
        if (save) {
            const esp_err_t save_err = credentials_save(ssid, password);
            ctx->snapshot.credentials_saved = save_err == ESP_OK;
            if (save_err != ESP_OK) {
                ctx->snapshot.last_error = save_err;
            }
        }
        memset(password, 0, sizeof(password));
        memset(ctx->pending_password, 0, sizeof(ctx->pending_password));
        xSemaphoreGive(ctx->lock);
        mdns_start(ctx);
        char connected_ssid[FACTORY_NETWORK_SSID_BYTES];
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        strlcpy(connected_ssid, ctx->snapshot.ssid, sizeof(connected_ssid));
        xSemaphoreGive(ctx->lock);
        ESP_LOGI(TAG, "factory Wi-Fi connected: SSID=%s IP=%s", connected_ssid, ip);
    }
}

/* The snapshot mutex survives failed attempts: UI/USB readers and any event
 * callback already in flight must never observe a deleted semaphore. The
 * shared event loop and esp-netif core also belong to the whole firmware. */
static void cleanup_failed_start(factory_network_context_t *ctx)
{
    if (ctx->wifi_events) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, ctx->wifi_events);
        ctx->wifi_events = NULL;
    }
    if (ctx->ip_events) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ctx->ip_events);
        ctx->ip_events = NULL;
    }
    if (ctx->wifi_initialized) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        ctx->wifi_initialized = false;
    }
    if (ctx->netif) {
        esp_netif_destroy_default_wifi(ctx->netif);
        ctx->netif = NULL;
    }
}

static esp_err_t start_network(factory_network_context_t *ctx)
{
    if (!ctx->lock)
        ctx->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(ctx->lock, ESP_ERR_NO_MEM, TAG, "create network mutex");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    const bool started = ctx->snapshot.started;
    xSemaphoreGive(ctx->lock);
    if (started)
        return ESP_OK;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        goto fail;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        goto fail;
    ctx->netif = esp_netif_create_default_wifi_sta();
    if (!ctx->netif) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_GOTO_ON_ERROR(esp_wifi_init(&config), fail, TAG, "initialize Wi-Fi");
    ctx->wifi_initialized = true;
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          network_event, ctx, &ctx->wifi_events),
                      fail, TAG, "register Wi-Fi events");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          network_event, ctx, &ctx->ip_events),
                      fail, TAG, "register IP events");
    ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), fail, TAG, "set Wi-Fi storage");
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), fail, TAG, "set station mode");
    /* Maintenance mode prioritizes responsive transfers over modem sleep.
     * Keep this policy local to Vibe Mode; applications own their power policy. */
    ESP_GOTO_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), fail, TAG, "disable Wi-Fi modem sleep");
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "start Wi-Fi");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->snapshot.started = true;
    ctx->snapshot.state = FACTORY_NETWORK_NO_CREDENTIALS;
    ctx->snapshot.last_error = ESP_OK;
    xSemaphoreGive(ctx->lock);

    char ssid[FACTORY_NETWORK_SSID_BYTES] = {0};
    char password[FACTORY_NETWORK_PASSWORD_BYTES] = {0};
    ret = credentials_load(ssid, password);
    if (ret == ESP_OK && ssid[0] != '\0') {
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        ctx->snapshot.credentials_saved = true;
        xSemaphoreGive(ctx->lock);
        ret = apply_station_config(ctx, ssid, password);
        if (ret != ESP_OK)
            snapshot_set_error(ctx, ret, FACTORY_NETWORK_FAILED);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        snapshot_set_error(ctx, ret, FACTORY_NETWORK_FAILED);
    }
    /* A bad saved connection does not invalidate the initialized driver:
     * provisioning a different network must remain possible. */
    memset(password, 0, sizeof(password));
    (void)request_scan_started(ctx);
    return ESP_OK;
fail:
    cleanup_failed_start(ctx);
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->snapshot.started = false;
    ctx->snapshot.scanning = false;
    ctx->snapshot.state = FACTORY_NETWORK_FAILED;
    ctx->snapshot.last_error = ret;
    xSemaphoreGive(ctx->lock);
    return ret;
}

esp_err_t factory_network_start(void)
{
    factory_network_context_t *ctx = &s_network;
    /* Startup may be retried by the render task or USB provisioning. Do not
     * let two callers initialize or tear down the same Wi-Fi resources. */
    if (atomic_flag_test_and_set(&ctx->starting))
        return ESP_ERR_INVALID_STATE;
    const esp_err_t err = start_network(ctx);
    atomic_flag_clear(&ctx->starting);
    return err;
}

static esp_err_t request_scan_started(factory_network_context_t *ctx)
{
    ESP_RETURN_ON_FALSE(ctx->lock, ESP_ERR_INVALID_STATE, TAG, "factory network is not started");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (ctx->snapshot.scanning) {
        xSemaphoreGive(ctx->lock);
        return ESP_OK;
    }
    ctx->snapshot.scanning = true;
    xSemaphoreGive(ctx->lock);
    const esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        ctx->snapshot.scanning = false;
        ctx->snapshot.last_error = err;
        xSemaphoreGive(ctx->lock);
    }
    return err;
}

esp_err_t factory_network_request_scan(void)
{
    factory_network_context_t *ctx = &s_network;
    ESP_RETURN_ON_ERROR(factory_network_start(), TAG, "start network for scan");
    return request_scan_started(ctx);
}

esp_err_t factory_network_connect(const char *ssid, const char *password)
{
    factory_network_context_t *ctx = &s_network;
    ESP_RETURN_ON_FALSE(ssid && password, ESP_ERR_INVALID_ARG, TAG, "invalid connection request");
    const size_t ssid_size = strnlen(ssid, FACTORY_NETWORK_SSID_BYTES);
    const size_t password_size = strnlen(password, FACTORY_NETWORK_PASSWORD_BYTES);
    bool password_valid = password_size == 0 || (password_size >= 8 && password_size <= 63);
    if (password_size == 64) {
        password_valid = true;
        for (size_t i = 0; i < password_size; ++i) {
            const char character = password[i];
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F'))) {
                password_valid = false;
                break;
            }
        }
    }
    ESP_RETURN_ON_FALSE(ssid_size > 0 && ssid_size < FACTORY_NETWORK_SSID_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid SSID length");
    ESP_RETURN_ON_FALSE(password_valid, ESP_ERR_INVALID_SIZE, TAG, "invalid password length");

    ESP_RETURN_ON_ERROR(factory_network_start(), TAG, "start network for provisioning");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    strlcpy(ctx->pending_ssid, ssid, sizeof(ctx->pending_ssid));
    strlcpy(ctx->pending_password, password, sizeof(ctx->pending_password));
    ctx->pending_credentials = true;
    xSemaphoreGive(ctx->lock);
    (void)esp_wifi_disconnect();
    return apply_station_config(ctx, ssid, password);
}

esp_err_t factory_network_forget(void)
{
    factory_network_context_t *ctx = &s_network;
    if (!ctx->lock)
        return credentials_erase();
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    esp_err_t err = credentials_erase();
    if (err != ESP_OK) { xSemaphoreGive(ctx->lock); return err; }
    ctx->pending_credentials = false;
    memset(ctx->pending_ssid, 0, sizeof(ctx->pending_ssid));
    memset(ctx->pending_password, 0, sizeof(ctx->pending_password));
    ctx->snapshot.credentials_saved = false;
    ctx->snapshot.ssid[0] = '\0';
    ctx->snapshot.ip[0] = '\0';
    ctx->snapshot.state = FACTORY_NETWORK_NO_CREDENTIALS;
    ctx->snapshot.last_error = ESP_OK;
    bool suspended = ctx->suspended;
    xSemaphoreGive(ctx->lock);
    (void)esp_wifi_disconnect();
    return suspended ? ESP_OK : factory_network_request_scan();
}

esp_err_t factory_network_suspend(bool suspended)
{
    factory_network_context_t *ctx = &s_network;
    if (!ctx->lock || !ctx->wifi_initialized)
        return suspended ? ESP_OK : factory_network_start();
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    bool changed = ctx->suspended != suspended;
    ctx->suspended = suspended;
    if (suspended) {
        ctx->snapshot.state = FACTORY_NETWORK_STOPPED;
        ctx->snapshot.ip[0] = 0;
        ctx->snapshot.scanning = false;
    }
    xSemaphoreGive(ctx->lock);
    if (!changed) return ESP_OK;
    if (suspended) return esp_wifi_stop();
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) return err;
    char ssid[FACTORY_NETWORK_SSID_BYTES] = {0};
    char password[FACTORY_NETWORK_PASSWORD_BYTES] = {0};
    if (credentials_load(ssid, password) == ESP_OK)
        err = apply_station_config(ctx, ssid, password);
    memset(password, 0, sizeof(password));
    return err;
}

esp_err_t factory_network_get_snapshot(factory_network_snapshot_t *snapshot)
{
    factory_network_context_t *ctx = &s_network;
    ESP_RETURN_ON_FALSE(snapshot, ESP_ERR_INVALID_ARG, TAG, "snapshot is null");
    ESP_RETURN_ON_FALSE(ctx->lock, ESP_ERR_INVALID_STATE, TAG, "factory network is not started");
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    *snapshot = ctx->snapshot;
    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}
