// SPDX-License-Identifier: Apache-2.0
#include "vibe_device.h"
#include "vibe_battery.h"
#include "vibe_settings.h"
#include "factory_network.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
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
static unsigned s_pattern_q[8], s_q_head, s_q_tail;
static esp_timer_handle_t s_motor_timer;
static SemaphoreHandle_t s_motor_lock;
static bool s_motor_shutdown; /* Protected by s_motor_lock, including timer callbacks. */
/* Publish each peripheral only after its driver and handles are usable. */
static unsigned s_ready;
enum { READY_HAPTICS = 1, READY_IMU = 2, READY_BATTERY = 4, READY_PERIPHERALS = 8 };
enum { MODE_NONE, MODE_CONTROLS, MODE_TELEMETRY };
static unsigned s_mode;
static bool s_starting;

static void peripheral_ready(unsigned mask)
{
    taskENTER_CRITICAL(&s_lock);
    s_ready |= mask;
    taskEXIT_CRITICAL(&s_lock);
}
/* Single 10 ms pulse for press/release; distinct patterns for reject/success/error. */
static const unsigned s_patterns[][3] = {
    {10, 0, 0}, {10, 45, 10}, {24, 0, 0}, {14, 70, 22}, {40, 60, 40},
};
static unsigned s_motor_pattern, s_motor_step;
static void motor_shutdown(void)
{
    /* App launches, OTA and explicit restarts all pass through esp_restart().
     * A stopped timer may already have a callback waiting on this lock, and
     * the UI worker can still hold a queued pulse. Neither may re-enable PWM. */
    xSemaphoreTake(s_motor_lock, portMAX_DELAY);
    s_motor_shutdown = true;
    if (s_motor_timer) (void)esp_timer_stop(s_motor_timer);
    const esp_err_t err = bsp_motor_set(false);
    if (err != ESP_OK) ESP_LOGW("vibe_device", "Motor shutdown failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_motor_lock);
}
static void motor_step(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_motor_lock, portMAX_DELAY);
    if (s_motor_shutdown) { xSemaphoreGive(s_motor_lock); return; }
    unsigned step = ++s_motor_step;
    unsigned duration = step < 3 ? s_patterns[s_motor_pattern][step] : 0;
    (void)bsp_motor_set(duration && !(step & 1));
    if (duration) (void)esp_timer_start_once(s_motor_timer, duration * 1000);
    xSemaphoreGive(s_motor_lock);
}
static void motor_play(unsigned pattern)
{
    xSemaphoreTake(s_motor_lock, portMAX_DELAY);
    if (s_motor_shutdown) { xSemaphoreGive(s_motor_lock); return; }
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
    if (s_mode == MODE_CONTROLS) {
        s_asleep = asleep; s_rotate_allowed = rotation_allowed;
    }
    taskEXIT_CRITICAL(&s_lock);
}
void vibe_device_haptic(unsigned pattern)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_mode == MODE_CONTROLS) {
        unsigned p = pattern ? pattern : 1;
        if (s_q_tail - s_q_head < 8)
            s_pattern_q[s_q_tail++ & 7] = p;
        else
            s_pattern_q[(s_q_tail - 1) & 7] = p;
    }
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
/* BSP exposes measured BQ27220 state, not cable presence. A missing gauge or
 * impossible/absent battery sample must not become a full/empty battery icon. */
static void read_battery(vibe_device_snapshot_t *snapshot)
{
    vibe_battery_sample_t status = {0};
    const unsigned battery_present = 1U << 3; /* BQ27220 BatteryStatus.BATTPRES */
    const unsigned discharging = 1U << 0;     /* BQ27220 BatteryStatus.DSG */
    const unsigned full_charged = 1U << 9;    /* BQ27220 BatteryStatus.FC */
    const esp_err_t error = vibe_battery_read(&status);
    const bool valid = error == ESP_OK && status.voltage_mv > 0 &&
        status.state_of_charge <= 100 && (status.status_flags & battery_present);
    snapshot->battery_valid = valid;
    snapshot->battery_percent = valid ? status.state_of_charge : 0;
    snapshot->charging = valid && !(status.status_flags & discharging) &&
        (status.current_ma > 0 || (snapshot->usb_connected && (status.status_flags & full_charged)));
    static unsigned last_state = ~0U;
    static esp_err_t last_error;
    const unsigned state = snapshot->battery_percent | (valid << 8) |
        (snapshot->charging << 9) | (snapshot->usb_connected << 10);
    if (state != last_state || error != last_error) {
        ESP_LOGW("vibe_device", "power usb=%u known=%u soc=%u charge=%u mv=%d ma=%d flags=%04x err=%x",
                 snapshot->usb_connected, valid, snapshot->battery_percent, snapshot->charging,
                 status.voltage_mv, status.current_ma, status.status_flags, (unsigned)error);
        last_state = state; last_error = error;
    }
}
static bool usb_link_active(bool previous)
{
    const bool mounted = tud_mounted(), suspended = tud_suspended();
    const bool active = mounted && !suspended;
    if (active != previous)
        ESP_LOGW("vibe_device", "usb active=%u mounted=%u suspended=%u", active, mounted, suspended);
    return active;
}

static void battery_task(void *unused)
{
    (void)unused;
    if (vibe_battery_init() == ESP_OK) peripheral_ready(READY_BATTERY);
    peripheral_ready(READY_PERIPHERALS);
    vTaskDelete(NULL);
}

static void telemetry_task(void *unused)
{
    (void)unused;
    if (xTaskCreate(battery_task, "vibe_battery", 4096, NULL, 1, NULL) != pdPASS) {
        peripheral_ready(READY_PERIPHERALS);
        ESP_LOGW("vibe_device", "Battery initialization worker unavailable");
    }
    vibe_device_snapshot_t snapshot = {0};
    int64_t next_battery = 0;
    while (true) {
        taskENTER_CRITICAL(&s_lock);
        const unsigned ready = s_ready;
        taskEXIT_CRITICAL(&s_lock);
        const bool usb_active = usb_link_active(snapshot.usb_connected);
        if (snapshot.usb_connected != usb_active) next_battery = 0;
        snapshot.usb_connected = usb_active;
        snapshot.peripherals_ready = (ready & READY_PERIPHERALS) != 0;
        const int64_t now = esp_timer_get_time();
        if ((ready & READY_BATTERY) && now >= next_battery) {
            read_battery(&snapshot);
            next_battery = now + 2000000;
        }
        taskENTER_CRITICAL(&s_lock); s_snapshot = snapshot; taskEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t reserve_mode(unsigned mode, bool *already_started)
{
    taskENTER_CRITICAL(&s_lock);
    const unsigned current = s_mode;
    const bool starting = s_starting;
    if (!current) { s_mode = mode; s_starting = true; }
    taskEXIT_CRITICAL(&s_lock);
    *already_started = current == mode && !starting;
    return current && (current != mode || starting) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void release_mode(void)
{
    taskENTER_CRITICAL(&s_lock); s_mode = MODE_NONE; s_starting = false; taskEXIT_CRITICAL(&s_lock);
}

static void started_mode(void)
{
    taskENTER_CRITICAL(&s_lock); s_starting = false; taskEXIT_CRITICAL(&s_lock);
}

esp_err_t vibe_device_start_telemetry(void)
{
    bool started = false;
    const esp_err_t err = reserve_mode(MODE_TELEMETRY, &started);
    if (err != ESP_OK || started) return err;
    if (xTaskCreate(telemetry_task, "vibe_telemetry", 3072, NULL, 2, NULL) != pdPASS) {
        release_mode();
        return ESP_ERR_NO_MEM;
    }
    started_mode();
    return ESP_OK;
}

static void peripheral_task(void *unused)
{
    (void)unused;
    s_motor_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t motor_timer = {.callback = motor_step, .name = "vibe_haptic"};
    if (s_motor_lock && bsp_motor_init() == ESP_OK) {
        xSemaphoreTake(s_motor_lock, portMAX_DELAY);
        esp_err_t err = esp_register_shutdown_handler(motor_shutdown);
        if (err == ESP_OK) err = esp_timer_create(&motor_timer, &s_motor_timer);
        if (err == ESP_OK) peripheral_ready(READY_HAPTICS);
        else ESP_LOGW("vibe_device", "Haptics unavailable: %s", esp_err_to_name(err));
        xSemaphoreGive(s_motor_lock);
    }
    const bsp_imu_config_t imu_config = BSP_IMU_CONFIG_DEFAULT();
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (bsp_imu_init() == ESP_OK) {
            if (bsp_imu_start(&imu_config) == ESP_OK) {
                peripheral_ready(READY_IMU);
                break;
            }
            (void)bsp_imu_deinit();
        }
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (vibe_battery_init() == ESP_OK)
        peripheral_ready(READY_BATTERY);
    peripheral_ready(READY_PERIPHERALS);
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
    if (xTaskCreate(peripheral_task, "vibe_peripherals", 6144, NULL, 1, NULL) != pdPASS) {
        peripheral_ready(READY_PERIPHERALS);
        ESP_LOGW("vibe_device", "Optional peripheral initialization unavailable");
    }
    int64_t next_battery = 0, candidate_since = 0, next_haptic = 0;
    unsigned candidate = 0, brightness = 0;
    bool sleeping = false, network_retry = false;
    int64_t next_network = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        (void)vibe_settings_snapshot(&s_preferences);
        taskENTER_CRITICAL(&s_lock);
        bool asleep = s_asleep, allowed = s_rotate_allowed;
        unsigned ready = s_ready;
        unsigned pattern = 0;
        if (!s_preferences.haptics) {
            s_q_head = s_q_tail = 0;
        } else if (s_q_head != s_q_tail && now >= next_haptic) {
            pattern = s_pattern_q[s_q_head++ & 7];
        }
        taskEXIT_CRITICAL(&s_lock);
        if ((asleep != sleeping || network_retry) && now >= next_network) {
            network_retry = factory_network_suspend(asleep) != ESP_OK;
            if (!network_retry) sleeping = asleep;
            next_network = network_retry ? now + 1000000 : 0;
        }
        unsigned level = asleep ? 10 : s_preferences.brightness ? s_preferences.brightness : 80;
        if (level != brightness && s_brightness && s_brightness(level) == ESP_OK) {
            if (!brightness)
                ESP_LOGI("boot_perf", "brightness_ready_at_us=%lld", (long long)esp_timer_get_time());
            brightness = level;
        }
        const bool usb_active = usb_link_active(snapshot.usb_connected);
        if (snapshot.usb_connected != usb_active) next_battery = 0;
        snapshot.usb_connected = usb_active;
        snapshot.haptics_available = (ready & READY_HAPTICS) != 0;
        snapshot.imu_available = (ready & READY_IMU) != 0;
        snapshot.peripherals_ready = (ready & READY_PERIPHERALS) != 0;
        if ((ready & READY_BATTERY) && now >= next_battery) {
            read_battery(&snapshot);
            next_battery = now + 2000000;
        }
        if (snapshot.haptics_available && (!s_preferences.haptics || pattern)) {
            motor_play(s_preferences.haptics ? pattern : 0);
            if (s_preferences.haptics && pattern == 1) next_haptic = now + 35000;
        }
        /* A fixed upright preference does not depend on live IMU readings. */
        unsigned wanted = s_preferences.auto_rotate ? snapshot.orientation : 0;
        if (!asleep && snapshot.imu_available) {
            float x, y, z;
            if (bsp_imu_get_accel(&x, &y, &z) == ESP_OK) {
                if (s_preferences.auto_rotate && fabsf(z) < 0.8f) {
                    /* Dominant-axis threshold provides angular hysteresis. */
                    if (fabsf(x) > fabsf(y) * 1.7f) wanted = x > 0 ? 90 : 270;
                    else if (fabsf(y) > fabsf(x) * 1.7f) wanted = y > 0 ? 0 : 180;
                }
            }
        }
        if (wanted != candidate) { candidate = wanted; candidate_since = now; }
        if (allowed && !asleep && candidate != snapshot.orientation && now - candidate_since >= 300000) {
            if (s_rotate && s_rotate(candidate) == ESP_OK) snapshot.orientation = candidate;
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
    bool started = false;
    const esp_err_t err = reserve_mode(MODE_CONTROLS, &started);
    if (err != ESP_OK || started) return err;
    s_rotate = rotate;
    s_brightness = brightness;
    s_keys = xQueueCreate(16, sizeof(unsigned));
    if (!s_keys) { release_mode(); return ESP_ERR_NO_MEM; }
    if (xTaskCreate(sample_task, "vibe_device", 6144, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_keys); s_keys = NULL; release_mode(); return ESP_ERR_NO_MEM;
    }
    started_mode();
    return ESP_OK;
}
