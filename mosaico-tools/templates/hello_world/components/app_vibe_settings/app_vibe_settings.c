// SPDX-License-Identifier: Apache-2.0

#include "app_vibe_settings.h"

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "bsp/button.h"
#include "bsp/esp_mosaico.h"
#include "bsp/imu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "iris_ota_support.h"
#include "mosaico_shared.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SYSMETA_PART "sysmeta"
#define PREFS_NS "vibe_prefs"
#define SHARED_NS "shared"
#define APP_NS "hello"
#define SETTINGS_STACK 6144
#define SHARED_SHOWN 8

static const char *TAG = "hello_settings";

typedef struct {
    uint8_t cmd;
    uint32_t value;
} pref_cmd_t;

static QueueHandle_t s_queue;
static app_vibe_settings_config_t s_config;
static volatile int64_t s_activity_us;
static bool s_dimmed;
static uint8_t s_brightness = 80;
static uint16_t s_timeout_s;
static bool s_rotate;
static bool s_haptics = true;
static uint8_t s_lang = 1;
static uint8_t s_volume = 80;
static bool s_imu;
static unsigned s_orientation;
static unsigned s_candidate;
static int64_t s_candidate_us;
static char s_password[65];
static char s_value[257];
static app_vibe_settings_view_t s_view;

static bool group_on(bool flag)
{
    const app_vibe_settings_config_t *c = &s_config;
    if (!c->wifi && !c->display && !c->language && !c->haptics && !c->shared) {
        return true;
    }
    return flag;
}

static bool name_secret(const char *key)
{
    char lower[16];
    size_t i = 0;
    if (!key) {
        return false;
    }
    for (; key[i] && i + 1 < sizeof(lower); ++i) {
        char c = key[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[i] = '\0';
    return strstr(lower, "key") || strstr(lower, "token") || strstr(lower, "secret") ||
           strstr(lower, "pass") || strstr(lower, "pwd");
}

static uint8_t snap_brightness(uint32_t value)
{
    static const uint8_t steps[] = {20, 40, 60, 80, 100};
    uint8_t best = 80;
    uint32_t best_gap = 1000;
    for (size_t i = 0; i < sizeof(steps); ++i) {
        uint32_t gap = value > steps[i] ? value - steps[i] : steps[i] - value;
        if (gap < best_gap) {
            best_gap = gap;
            best = steps[i];
        }
    }
    return best;
}

static uint16_t shown_timeout(uint32_t value)
{
    if (value == 0 || value == 60 || value == 180 || value == 300 || value == 600) {
        return (uint16_t)value;
    }
    return 0;
}

static bool cmd_accepted(app_vibe_cmd_t cmd, uint32_t value)
{
    switch (cmd) {
    case APP_VIBE_CMD_BRIGHTNESS:
        return value >= 20 && value <= 100 && value % 20 == 0;
    case APP_VIBE_CMD_TIMEOUT:
        return value == 0 || value == 60 || value == 180 || value == 300 || value == 600;
    case APP_VIBE_CMD_ROTATE:
    case APP_VIBE_CMD_HAPTICS:
        return value <= 1;
    case APP_VIBE_CMD_LANG:
        return value == 1 || value == 2;
    case APP_VIBE_CMD_VOLUME:
        return value <= 100 && value % 20 == 0;
    default:
        return false;
    }
}

static esp_err_t write_app_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t write_pref(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(SYSMETA_PART, PREFS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void load_prefs(void)
{
    nvs_handle_t handle;
    uint32_t value = 0;
    uint8_t stored = 0;
    (void)nvs_flash_init_partition(SYSMETA_PART);
    if (nvs_open_from_partition(SYSMETA_PART, PREFS_NS, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u32(handle, "brightness", &value) == ESP_OK) {
            s_brightness = snap_brightness(value);
        }
        if (nvs_get_u32(handle, "timeout", &value) == ESP_OK) {
            s_timeout_s = shown_timeout(value);
        }
        if (nvs_get_u32(handle, "rotate", &value) == ESP_OK) {
            s_rotate = value == 1;
        }
        if (nvs_get_u32(handle, "haptics", &value) == ESP_OK) {
            s_haptics = value != 0;
        }
        if (nvs_get_u32(handle, "lang", &value) == ESP_OK && (value == 1 || value == 2)) {
            s_lang = (uint8_t)value;
        }
        nvs_close(handle);
    }
    if (nvs_open(APP_NS, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "volume", &stored) == ESP_OK && stored <= 100 && stored % 20 == 0) {
            s_volume = stored;
        }
        nvs_close(handle);
    }
}

static void fill_ascii(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    if (!src) {
        src = "";
    }
    for (size_t i = 0; src[i] && o + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void fill_shared(void)
{
    nvs_iterator_t it = NULL;
    s_view.shared_count = 0;
    if (!group_on(s_config.shared)) {
        return;
    }
    if (nvs_entry_find(SYSMETA_PART, SHARED_NS, NVS_TYPE_STR, &it) != ESP_OK) {
        return;
    }
    while (it && s_view.shared_count < SHARED_SHOWN) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) != ESP_OK) {
            break;
        }
        if (strcmp(info.key, "wifi_ssid") != 0 && strcmp(info.key, "wifi_password") != 0) {
            char shown[24];
            s_value[0] = '\0';
            if (name_secret(info.key) ||
                mosaico_shared_get(info.key, s_value, sizeof(s_value)) != ESP_OK) {
                snprintf(s_view.shared_line[s_view.shared_count],
                         sizeof(s_view.shared_line[0]), "%s  ****", info.key);
            } else {
                fill_ascii(shown, sizeof(shown), s_value);
                snprintf(s_view.shared_line[s_view.shared_count],
                         sizeof(s_view.shared_line[0]), "%s  %s", info.key,
                         shown[0] ? shown : "-");
            }
            s_view.shared_count++;
        }
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
    }
    nvs_release_iterator(it);
    memset(s_value, 0, sizeof(s_value));
}

static void publish_view(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.brightness = s_brightness;
    s_view.timeout_s = s_timeout_s;
    s_view.rotate = s_rotate;
    s_view.haptics = s_haptics;
    s_view.lang = s_lang;
    s_view.volume = s_volume;
    if (group_on(s_config.wifi)) {
        app_wifi_copy(s_view.wifi_ap, s_view.wifi_open, &s_view.wifi_ap_count);
        app_wifi_status(s_view.wifi_line, sizeof(s_view.wifi_line), s_lang);
        app_wifi_detail(s_view.wifi_detail, sizeof(s_view.wifi_detail), s_lang);
        snprintf(s_view.wifi_ssid, sizeof(s_view.wifi_ssid), "%s", app_wifi_selected());
    }
    fill_shared();
    if (s_config.on_view) {
        s_config.on_view(&s_view, s_config.ctx);
    }
}

static void apply_out(app_vibe_apply_t what, uint32_t value)
{
    if (s_config.on_apply) {
        s_config.on_apply(what, value, s_config.ctx);
    }
}

static void haptic_feedback(void)
{
    if (s_haptics) {
        apply_out(APP_VIBE_APPLY_HAPTIC_FEEDBACK, 1);
    }
}

static unsigned held_orientation(void)
{
    float x = 0;
    float y = 0;
    float z = 0;
    float ax;
    float ay;
    float az;
    if (!s_imu || s_dimmed) {
        return s_orientation;
    }
    if (bsp_imu_get_accel(&x, &y, &z) != ESP_OK) {
        return s_orientation;
    }
    ax = x < 0 ? -x : x;
    ay = y < 0 ? -y : y;
    az = z < 0 ? -z : z;
    if (az >= 0.8f) {
        return s_orientation;
    }
    if (ax > ay * 1.7f) {
        return x > 0 ? 90 : 270;
    }
    if (ay > ax * 1.7f) {
        return y > 0 ? 0 : 180;
    }
    return s_orientation;
}

static void track_orientation(int64_t now)
{
    unsigned wanted = s_rotate ? held_orientation() : 0;
    if (wanted != s_candidate) {
        s_candidate = wanted;
        s_candidate_us = now;
    }
    if (s_candidate != s_orientation && now - s_candidate_us >= 300000) {
        s_orientation = s_candidate;
        apply_out(APP_VIBE_APPLY_ROTATION, s_orientation);
    }
}

static void apply_command(const pref_cmd_t *cmd)
{
    app_vibe_cmd_t kind = (app_vibe_cmd_t)cmd->cmd;
    uint32_t value = cmd->value;
    const char *key = NULL;
    if (kind == APP_VIBE_CMD_HAPTIC_FEEDBACK) {
        haptic_feedback();
        return;
    }
    if (kind != APP_VIBE_CMD_HAPTICS) {
        haptic_feedback();
    }
    if (kind == APP_VIBE_CMD_RELOAD || kind == APP_VIBE_CMD_WIFI_SCAN) {
        if (kind == APP_VIBE_CMD_WIFI_SCAN && group_on(s_config.wifi)) {
            (void)app_wifi_scan(NULL, NULL);
        }
        publish_view();
        return;
    }
    if (kind == APP_VIBE_CMD_WIFI_PICK && group_on(s_config.wifi)) {
        (void)app_wifi_select((uint8_t)value);
        publish_view();
        return;
    }
    if (kind == APP_VIBE_CMD_WIFI_JOIN && group_on(s_config.wifi)) {
        app_wifi_set_password(s_password);
        memset(s_password, 0, sizeof(s_password));
        (void)app_wifi_join();
        publish_view();
        return;
    }
    if (kind == APP_VIBE_CMD_WIFI_FORGET && group_on(s_config.wifi)) {
        (void)app_wifi_forget();
        publish_view();
        return;
    }
    if (kind == APP_VIBE_CMD_VOLUME) {
        if (!cmd_accepted(kind, value) || write_app_u8("volume", (uint8_t)value) != ESP_OK) {
            return;
        }
        s_volume = (uint8_t)value;
        publish_view();
        return;
    }
    if (!cmd_accepted(kind, value)) {
        return;
    }
    if (kind == APP_VIBE_CMD_BRIGHTNESS && !group_on(s_config.display)) {
        return;
    }
    if ((kind == APP_VIBE_CMD_TIMEOUT || kind == APP_VIBE_CMD_ROTATE) &&
        !group_on(s_config.display)) {
        return;
    }
    if (kind == APP_VIBE_CMD_HAPTICS && !group_on(s_config.haptics)) {
        return;
    }
    if (kind == APP_VIBE_CMD_LANG && !group_on(s_config.language)) {
        return;
    }
    if (kind == APP_VIBE_CMD_ROTATE) {
        value = value ? 1U : 0U;
        key = "rotate";
    } else if (kind == APP_VIBE_CMD_HAPTICS) {
        value = value ? 1U : 0U;
        key = "haptics";
    } else if (kind == APP_VIBE_CMD_BRIGHTNESS) {
        key = "brightness";
    } else if (kind == APP_VIBE_CMD_TIMEOUT) {
        key = "timeout";
    } else if (kind == APP_VIBE_CMD_LANG) {
        key = "lang";
    }
    if (!key || write_pref(key, value) != ESP_OK) {
        return;
    }
    if (kind == APP_VIBE_CMD_BRIGHTNESS) {
        s_brightness = (uint8_t)value;
        s_dimmed = false;
        apply_out(APP_VIBE_APPLY_BRIGHTNESS, s_brightness);
    } else if (kind == APP_VIBE_CMD_TIMEOUT) {
        s_timeout_s = (uint16_t)value;
    } else if (kind == APP_VIBE_CMD_ROTATE) {
        s_rotate = value == 1;
        s_candidate = s_rotate ? s_orientation : 0;
        s_candidate_us = esp_timer_get_time() - 300000;
    } else if (kind == APP_VIBE_CMD_HAPTICS) {
        s_haptics = value == 1;
        apply_out(APP_VIBE_APPLY_HAPTICS, s_haptics ? 1U : 0U);
    } else if (kind == APP_VIBE_CMD_LANG) {
        s_lang = (uint8_t)value;
    }
    publish_view();
}

static void on_ai(void *button, void *ctx)
{
    (void)button;
    (void)ctx;
    if (s_config.on_toggle) {
        s_config.on_toggle(s_config.ctx);
    }
}

static void on_ai_hold(void *button, void *ctx)
{
    (void)button;
    (void)ctx;
    (void)iris_ota_support_enter_recovery();
}

static void settings_task(void *arg)
{
    (void)arg;
    load_prefs();
    s_activity_us = esp_timer_get_time();
    if (bsp_imu_init() == ESP_OK) {
        const bsp_imu_config_t imu = BSP_IMU_CONFIG_DEFAULT();
        s_imu = bsp_imu_start(&imu) == ESP_OK;
    }
    if (group_on(s_config.wifi)) {
        (void)app_wifi_start();
    }
    apply_out(APP_VIBE_APPLY_BRIGHTNESS, s_brightness);
    publish_view();
    for (;;) {
        pref_cmd_t cmd;
        int64_t now;
        if (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
            apply_command(&cmd);
        }
        now = esp_timer_get_time();
        if (s_timeout_s > 0 && !s_dimmed &&
            now - s_activity_us >= (int64_t)s_timeout_s * 1000000) {
            s_dimmed = true;
            apply_out(APP_VIBE_APPLY_BRIGHTNESS, 10);
        } else if (s_dimmed && now - s_activity_us < (int64_t)s_timeout_s * 1000000) {
            s_dimmed = false;
            apply_out(APP_VIBE_APPLY_BRIGHTNESS, s_brightness);
        }
        if (group_on(s_config.display)) {
            track_orientation(now);
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

esp_err_t app_vibe_settings_start(const app_vibe_settings_config_t *config)
{
    button_handle_t buttons[BSP_BUTTON_NUM];
    int count = 0;
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_queue = xQueueCreate(8, sizeof(pref_cmd_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (bsp_iot_button_create(buttons, &count, BSP_BUTTON_NUM) == ESP_OK &&
        count > BSP_BUTTON_AI) {
        (void)iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK,
                                     NULL, on_ai, NULL);
        (void)iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START,
                                     NULL, on_ai_hold, NULL);
    } else {
        ESP_LOGW(TAG, "AI key unavailable; open settings from the scene");
    }
    if (xTaskCreate(settings_task, "hello_settings", SETTINGS_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_vibe_settings_request(app_vibe_cmd_t cmd, uint32_t value)
{
    pref_cmd_t item;
    if (!s_queue) {
        return;
    }
    item.cmd = (uint8_t)cmd;
    item.value = value;
    (void)xQueueSend(s_queue, &item, 0);
}

void app_vibe_settings_set_password(const char *password)
{
    size_t i = 0;
    memset(s_password, 0, sizeof(s_password));
    if (!password) {
        return;
    }
    while (password[i] && i < sizeof(s_password) - 1) {
        s_password[i] = password[i];
        i++;
    }
}

void app_vibe_settings_set_haptics(bool enabled)
{
    app_vibe_settings_request(APP_VIBE_CMD_HAPTICS, enabled ? 1U : 0U);
}

void app_vibe_settings_note_activity(void)
{
    s_activity_us = esp_timer_get_time();
}
