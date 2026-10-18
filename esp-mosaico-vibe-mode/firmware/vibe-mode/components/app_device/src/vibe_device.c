// SPDX-License-Identifier: Apache-2.0
#include "vibe_device.h"
#include "vibe_settings.h"
#include "factory_network.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include <math.h>
#include <string.h>

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_keys;
static vibe_device_snapshot_t s_snapshot;
static bool s_asleep, s_rotate_allowed = true;
static unsigned s_pattern;
static esp_timer_handle_t s_motor_timer;
static SemaphoreHandle_t s_motor_lock;
/* Publish each peripheral only after its driver and handles are usable. */
static unsigned s_ready;
enum { READY_HAPTICS = 1, READY_IMU = 2, READY_BATTERY = 4 };

static void peripheral_ready(unsigned mask)
{
    taskENTER_CRITICAL(&s_lock);
    s_ready |= mask;
    taskEXIT_CRITICAL(&s_lock);
}
/* BSP already drives the motor at 100%; strengthen feedback by doubling
 * the on-time of each pulse while preserving the inter-pulse pauses. */
static const unsigned s_patterns[][3] = {
    {16, 0, 0}, {16, 45, 16}, {48, 0, 0}, {28, 70, 44}, {80, 60, 80},
};
static unsigned s_motor_pattern, s_motor_step;
static void motor_step(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_motor_lock, portMAX_DELAY);
    unsigned step = ++s_motor_step;
    unsigned duration = step < 3 ? s_patterns[s_motor_pattern][step] : 0;
    (void)bsp_motor_set(duration && !(step & 1));
    if (duration) (void)esp_timer_start_once(s_motor_timer, duration * 1000);
    xSemaphoreGive(s_motor_lock);
}
static void motor_play(unsigned pattern)
{
    xSemaphoreTake(s_motor_lock, portMAX_DELAY);
    (void)esp_timer_stop(s_motor_timer);
    s_motor_pattern = pattern >= 1 && pattern <= 5 ? pattern - 1 : 0;
    s_motor_step = 0;
    (void)bsp_motor_set(pattern != 0);
    if (pattern) (void)esp_timer_start_once(s_motor_timer, s_patterns[s_motor_pattern][0] * 1000);
    xSemaphoreGive(s_motor_lock);
}
static esp_err_t (*s_rotate)(unsigned degrees);
static esp_err_t (*s_brightness)(unsigned percent);
static EXT_RAM_BSS_ATTR vibe_settings_snapshot_t s_preferences;

void vibe_device_policy(bool asleep, bool rotation_allowed)
{
    taskENTER_CRITICAL(&s_lock);
    s_asleep = asleep; s_rotate_allowed = rotation_allowed;
    taskEXIT_CRITICAL(&s_lock);
}
void vibe_device_haptic(unsigned pattern)
{
    taskENTER_CRITICAL(&s_lock);
    s_pattern = pattern ? pattern : 1;
    taskEXIT_CRITICAL(&s_lock);
}
esp_err_t vibe_device_snapshot(vibe_device_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock); *out = s_snapshot; taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
unsigned vibe_device_poll_key(void)
{
    unsigned key = 0;
    if (s_keys) (void)xQueueReceive(s_keys, &key, 0);
    return key;
}
static void peripheral_task(void *unused)
{
    (void)unused;
    s_motor_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t motor_timer = {.callback = motor_step, .name = "vibe_haptic"};
    if (s_motor_lock && bsp_motor_init() == ESP_OK &&
        esp_timer_create(&motor_timer, &s_motor_timer) == ESP_OK)
        peripheral_ready(READY_HAPTICS);
    const bsp_imu_config_t imu_config = BSP_IMU_CONFIG_DEFAULT();
    if (bsp_imu_init() == ESP_OK && bsp_imu_start(&imu_config) == ESP_OK)
        peripheral_ready(READY_IMU);
    /* Battery profile verification and sealing may take seconds. Neither
     * physical keys nor display preferences depend on this work. */
    if (bsp_battery_init() == ESP_OK)
        peripheral_ready(READY_BATTERY);
    ESP_LOGI("boot_perf", "peripherals_ready_at_us=%lld", (long long)esp_timer_get_time());
    vTaskDelete(NULL);
}

static void sample_task(void *unused)
{
    (void)unused;
    vibe_device_snapshot_t snapshot = {0};
    const gpio_num_t pins[] = {BSP_BUTTON_AI_GPIO, BSP_BUTTON_BOOT_GPIO};
    bool pressed[2] = {false, false};
    bool sampled[2] = {false, false};
    int64_t key_changed[2] = {0, 0};
    for (unsigned i = 0; i < 2; ++i) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << pins[i], .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE};
        (void)gpio_config(&cfg);
    }
    ESP_LOGI("boot_perf", "keys_ready_at_us=%lld", (long long)esp_timer_get_time());
    if (xTaskCreate(peripheral_task, "vibe_peripherals", 6144, NULL, 1, NULL) != pdPASS)
        ESP_LOGW("vibe_device", "Optional peripheral initialization unavailable");
    int64_t next_battery = 0, candidate_since = 0;
    unsigned candidate = 0, brightness = 0;
    bool sleeping = false;
    float last_x = 0, last_y = 0, last_z = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        (void)vibe_settings_snapshot(&s_preferences);
        taskENTER_CRITICAL(&s_lock);
        bool asleep = s_asleep, allowed = s_rotate_allowed;
        unsigned ready = s_ready;
        unsigned pattern = s_pattern;
        s_pattern = 0;
        taskEXIT_CRITICAL(&s_lock);
        if (asleep != sleeping) {
            (void)factory_network_suspend(asleep);
            sleeping = asleep;
        }
        unsigned level = asleep ? 10 : s_preferences.brightness ? s_preferences.brightness : 80;
        if (level != brightness && s_brightness && s_brightness(level) == ESP_OK) {
            if (!brightness)
                ESP_LOGI("boot_perf", "brightness_ready_at_us=%lld", (long long)esp_timer_get_time());
            brightness = level;
        }
        snapshot.usb_connected = tud_mounted();
        snapshot.haptics_available = (ready & READY_HAPTICS) != 0;
        snapshot.imu_available = (ready & READY_IMU) != 0;
        if ((ready & READY_BATTERY) && now >= next_battery) {
            bsp_battery_status_t status;
            snapshot.battery_valid = bsp_battery_read(&status) == ESP_OK;
            if (snapshot.battery_valid) {
                snapshot.battery_percent = status.state_of_charge;
                snapshot.charging = status.current_ma > 0;
            }
            next_battery = now + 10000000;
        }
        if (snapshot.haptics_available && (!s_preferences.haptics || pattern))
            motor_play(s_preferences.haptics ? pattern : 0);
        if (snapshot.imu_available) {
            float x, y, z;
            if (bsp_imu_get_accel(&x, &y, &z) == ESP_OK) {
                float change = fabsf(x - last_x) + fabsf(y - last_y) + fabsf(z - last_z);
                if (asleep && change > 0.25f) ++snapshot.motion_revision;
                last_x = x; last_y = y; last_z = z;
                unsigned wanted = snapshot.orientation;
                if (!s_preferences.auto_rotate) wanted = 0;
                else if (fabsf(z) < 0.8f) {
                    /* Dominant-axis threshold provides angular hysteresis. */
                    if (fabsf(x) > fabsf(y) * 1.7f) wanted = x > 0 ? 90 : 270;
                    else if (fabsf(y) > fabsf(x) * 1.7f) wanted = y > 0 ? 0 : 180;
                }
                if (wanted != candidate) { candidate = wanted; candidate_since = now; }
                if (allowed && !asleep && candidate != snapshot.orientation && now - candidate_since >= 300000) {
                    if (s_rotate && s_rotate(candidate) == ESP_OK) snapshot.orientation = candidate;
                }
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            bool down = gpio_get_level(pins[i]) == BSP_BUTTON_ACTIVE_LEVEL;
            if (down != sampled[i]) { sampled[i] = down; key_changed[i] = now; }
            if (down != pressed[i] && now - key_changed[i] >= 40000) {
                unsigned key = i * 2 + (down ? 1 : 2);
                /* If full, retry the transition next sample; never overwrite a key. */
                if (xQueueSend(s_keys, &key, 0) == pdTRUE) pressed[i] = down;
            }
        }
        taskENTER_CRITICAL(&s_lock); s_snapshot = snapshot; taskEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t vibe_device_start(esp_err_t (*rotate)(unsigned degrees),
                            esp_err_t (*brightness)(unsigned percent))
{
    if (s_keys) return ESP_OK;
    s_rotate = rotate;
    s_brightness = brightness;
    s_keys = xQueueCreate(16, sizeof(unsigned));
    if (!s_keys) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sample_task, "vibe_device", 6144, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_keys); s_keys = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
