// SPDX-License-Identifier: Apache-2.0

#include "app_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_shared.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SYSMETA_PART "sysmeta"
#define SHARED_NS "shared"

static const char *TAG = "hello_wifi";

static bool s_started;
static char s_ap[APP_WIFI_AP_MAX][33];
static uint8_t s_ap_count;
static bool s_ap_open[APP_WIFI_AP_MAX];
static int s_selected = -1;
static char s_password[65];
static uint8_t s_password_len;
static int s_state;
static esp_err_t s_last_error;

static bool password_ok(const char *password, size_t length, bool open)
{
    size_t hex = 0;
    if (open && length == 0) {
        return true;
    }
    if (length >= 8 && length <= 63) {
        return true;
    }
    if (length != 64) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char c = password[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            hex++;
        }
    }
    return hex == 64;
}

static esp_err_t store_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(SYSMETA_PART, SHARED_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t connect_sta(const char *ssid, const char *password)
{
    wifi_config_t config;
    size_t ssid_len = strnlen(ssid, 32);
    size_t pass_len = strnlen(password, 64);
    memset(&config, 0, sizeof(config));
    if (ssid_len == 0 || ssid_len > sizeof(config.sta.ssid) ||
        pass_len > sizeof(config.sta.password)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(config.sta.ssid, ssid, ssid_len);
    memcpy(config.sta.password, password, pass_len);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    return err;
}

static esp_err_t ensure_stack(void)
{
    esp_err_t err;
    if (s_started) {
        return ESP_OK;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        if (!esp_netif_create_default_wifi_sta()) {
            return ESP_ERR_NO_MEM;
        }
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t app_wifi_start(void)
{
    char ssid[33];
    char password[65];
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        s_last_error = err;
        ESP_LOGW(TAG, "wifi start %s", esp_err_to_name(err));
        return err;
    }
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    if (mosaico_shared_get("wifi_ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        memset(password, 0, sizeof(password));
        return ESP_OK;
    }
    if (mosaico_shared_get("wifi_password", password, sizeof(password)) != ESP_OK) {
        password[0] = '\0';
    }
    err = connect_sta(ssid, password);
    memset(password, 0, sizeof(password));
    s_state = err == ESP_OK ? 1 : 2;
    s_last_error = err;
    return err;
}

static bool sta_associated(void)
{
    /* Settings task is the only caller. Keep the record off that task's stack. */
    static wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static esp_err_t start_scan(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    (void)esp_wifi_scan_stop();
    return esp_wifi_scan_start(&cfg, true);
}

static void store_records(const wifi_ap_record_t *records, uint16_t found)
{
    s_ap_count = 0;
    s_selected = -1;
    for (uint16_t i = 0; i < found && s_ap_count < APP_WIFI_AP_MAX; ++i) {
        bool duplicate = false;
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        for (uint8_t j = 0; j < s_ap_count; ++j) {
            if (strncmp(s_ap[j], (const char *)records[i].ssid, 32) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        memset(s_ap[s_ap_count], 0, sizeof(s_ap[0]));
        memcpy(s_ap[s_ap_count], records[i].ssid, 32);
        s_ap_open[s_ap_count] = records[i].authmode == WIFI_AUTH_OPEN;
        s_ap_count++;
    }
}

static esp_err_t read_scan(wifi_ap_record_t *records, uint16_t limit)
{
    uint16_t found = 0;
    esp_err_t err = start_scan();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK || found == 0) {
        s_ap_count = 0;
        s_selected = -1;
        return err;
    }
    if (found > limit) {
        found = limit;
    }
    err = esp_wifi_scan_get_ap_records(&found, records);
    if (err == ESP_OK) {
        store_records(records, found);
    }
    return err;
}

static void resume_saved(void)
{
    char ssid[33];
    char password[65];
    esp_err_t err;
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    if (mosaico_shared_get("wifi_ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return;
    }
    if (mosaico_shared_get("wifi_password", password, sizeof(password)) != ESP_OK) {
        password[0] = '\0';
    }
    err = connect_sta(ssid, password);
    memset(password, 0, sizeof(password));
    memset(ssid, 0, sizeof(ssid));
    if (err == ESP_OK && s_state != 5) {
        s_state = 1;
    }
}

esp_err_t app_wifi_scan(char out[APP_WIFI_AP_MAX][33], uint8_t *count)
{
    wifi_ap_record_t *records;
    uint16_t limit = 20;
    bool resume = false;
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        s_last_error = err;
        s_state = 5;
        ESP_LOGW(TAG, "scan stack %s", esp_err_to_name(err));
        return err;
    }
    s_state = 4;
    /* A connect in progress makes scan_start return ESP_ERR_WIFI_STATE and
     * the list stays empty. An already associated STA can scan in place. */
    if (!sta_associated()) {
        resume = true;
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    records = calloc(limit, sizeof(*records));
    if (!records) {
        s_state = 5;
        if (resume) {
            resume_saved();
        }
        return ESP_ERR_NO_MEM;
    }
    err = read_scan(records, limit);
    if (err == ESP_ERR_WIFI_STATE || (err == ESP_OK && s_ap_count == 0 && !resume)) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        resume = true;
        err = read_scan(records, limit);
    }
    free(records);
    if (out && count) {
        memset(out, 0, APP_WIFI_AP_MAX * 33);
        for (uint8_t i = 0; i < s_ap_count; ++i) {
            memcpy(out[i], s_ap[i], 33);
        }
        *count = s_ap_count;
    }
    s_last_error = err;
    if (err != ESP_OK) {
        s_state = 5;
        ESP_LOGW(TAG, "scan %s", esp_err_to_name(err));
        if (resume) {
            resume_saved();
        }
        return err;
    }
    ESP_LOGI(TAG, "scan count %u", (unsigned)s_ap_count);
    s_state = 0;
    if (resume) {
        resume_saved();
    }
    return ESP_OK;
}

esp_err_t app_wifi_select(uint8_t index)
{
    if (index >= s_ap_count) {
        return ESP_ERR_INVALID_ARG;
    }
    s_selected = (int)index;
    return ESP_OK;
}

void app_wifi_set_password(const char *text)
{
    size_t len = 0;
    memset(s_password, 0, sizeof(s_password));
    s_password_len = 0;
    if (!text) {
        return;
    }
    while (text[len] && len < 64) {
        s_password[len] = text[len];
        len++;
    }
    s_password[len] = '\0';
    s_password_len = (uint8_t)len;
}

const char *app_wifi_selected(void)
{
    if (s_selected < 0 || (uint8_t)s_selected >= s_ap_count) {
        return "";
    }
    return s_ap[s_selected];
}

esp_err_t app_wifi_forget(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(SYSMETA_PART, SHARED_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        if (nvs_erase_key(handle, "wifi_ssid") == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK && nvs_erase_key(handle, "wifi_password") == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (s_started) {
        (void)esp_wifi_disconnect();
    }
    s_selected = -1;
    app_wifi_set_password(NULL);
    s_state = 0;
    return err;
}

esp_err_t app_wifi_join(void)
{
    bool open;
    esp_err_t err;
    if (s_selected < 0 || (uint8_t)s_selected >= s_ap_count) {
        s_state = 2;
        return ESP_ERR_INVALID_STATE;
    }
    open = s_ap_open[s_selected];
    if (!password_ok(s_password, s_password_len, open)) {
        s_state = 2;
        s_last_error = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }
    err = ensure_stack();
    if (err == ESP_OK) {
        err = store_credentials(s_ap[s_selected], s_password);
    }
    if (err == ESP_OK) {
        err = connect_sta(s_ap[s_selected], s_password);
    }
    memset(s_password, 0, sizeof(s_password));
    s_password_len = 0;
    s_last_error = err;
    s_state = err == ESP_OK ? 3 : 2;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "join %s", esp_err_to_name(err));
    }
    return err;
}

void app_wifi_copy(char out[APP_WIFI_AP_MAX][33], uint8_t *open, uint8_t *count)
{
    if (!out || !count) {
        return;
    }
    memset(out, 0, APP_WIFI_AP_MAX * 33);
    if (open) {
        memset(open, 0, APP_WIFI_AP_MAX);
    }
    for (uint8_t i = 0; i < s_ap_count; ++i) {
        memcpy(out[i], s_ap[i], 33);
        if (open) {
            open[i] = s_ap_open[i] ? 1 : 0;
        }
    }
    *count = s_ap_count;
}

void app_wifi_status(char *dst, size_t dst_len, uint8_t lang)
{
    const char *ssid = "";
    char saved[33];
    const bool zh = lang == 2;
    if (!dst || dst_len == 0) {
        return;
    }
    memset(saved, 0, sizeof(saved));
    if (s_selected >= 0) {
        ssid = s_ap[s_selected];
    } else if (mosaico_shared_get("wifi_ssid", saved, sizeof(saved)) == ESP_OK) {
        ssid = saved;
    }
    if (ssid[0]) {
        snprintf(dst, dst_len, "%s", ssid);
    } else {
        snprintf(dst, dst_len, zh ? "未连接" : "Not connected");
    }
    memset(saved, 0, sizeof(saved));
}

void app_wifi_detail(char *dst, size_t dst_len, uint8_t lang)
{
    const bool zh = lang == 2;
    if (!dst || dst_len == 0) {
        return;
    }
    if (s_state == 4) {
        snprintf(dst, dst_len, zh ? "正在扫描" : "Scanning");
    } else if (s_state == 5) {
        snprintf(dst, dst_len, zh ? "扫描失败" : "Scan failed");
    } else if (s_state == 1) {
        snprintf(dst, dst_len, zh ? "正在连接" : "Connecting");
    } else if (s_state == 2) {
        snprintf(dst, dst_len, zh ? "请检查密码后再试" : "Check the password");
    } else if (s_state == 3) {
        snprintf(dst, dst_len, zh ? "已保存" : "Saved");
    } else {
        snprintf(dst, dst_len, zh ? "请选择下方网络" : "Choose a network below");
    }
}

void app_wifi_pass_mask(char *dst, size_t dst_len)
{
    uint8_t stars;
    if (!dst || dst_len == 0) {
        return;
    }
    stars = s_password_len > 8 ? 8 : s_password_len;
    memset(dst, '*', stars);
    dst[stars] = '\0';
    if (stars == 0 && dst_len > 1) {
        dst[0] = '-';
        dst[1] = '\0';
    }
}
