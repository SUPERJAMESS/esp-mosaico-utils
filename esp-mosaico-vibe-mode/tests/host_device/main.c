// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "vibe_device.h"
#include "vibe_battery.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct test_queue { pthread_mutex_t lock; unsigned values[16], count; };
static struct test_queue keys = {.lock = PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t motor_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t threads[2];
static unsigned thread_count;
static atomic_bool stopping, release_battery, battery_entered, battery_initialized;
static atomic_int key_level = 1;
static atomic_uint brightness;
static atomic_uint network_calls;
static atomic_bool network_fail, network_sleeping;
static bool sensor_failure, task_failure, sample_failure, registration_failure, timer_failure;
static bool telemetry;
static atomic_bool usb_mounted = true, usb_suspended;
static atomic_int battery_error, battery_soc = 73, battery_current = 10, battery_voltage = 3900;
static atomic_uint battery_flags = 8;
static atomic_llong time_advance;
static atomic_uint battery_reads;
static atomic_bool auto_rotate, imu_read_failure, rotation_failure;
static atomic_uint sensor_orientation, rotation_calls;
static bool imu_retry;
static unsigned imu_attempts;
static atomic_bool motor_on, timer_active;
static atomic_uint motor_on_count, timer_start_count;
static shutdown_handler_t shutdown_handler;
static void (*timer_callback)(void *);

QueueHandle_t xQueueCreate(unsigned n, unsigned size) { assert(!telemetry); assert(n == 16 && size == sizeof(unsigned)); return &keys; }
void vQueueDelete(QueueHandle_t q) { assert(q == &keys); }
int xQueueReceive(QueueHandle_t q, void *out, unsigned wait) {
    pthread_mutex_lock(&q->lock);
    bool found = q->count != 0;
    if (found) { *(unsigned *)out = q->values[0]; --q->count; memmove(q->values, q->values + 1, q->count * sizeof(unsigned)); }
    pthread_mutex_unlock(&q->lock); return found;
}
int xQueueSend(QueueHandle_t q, const void *value, unsigned wait) {
    pthread_mutex_lock(&q->lock);
    bool room = q->count < 16;
    if (room) q->values[q->count++] = *(const unsigned *)value;
    pthread_mutex_unlock(&q->lock); return room;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &motor_lock; }
int xSemaphoreTake(SemaphoreHandle_t m, unsigned wait) { pthread_mutex_lock(m); return 1; }
void xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); }
struct task { void (*fn)(void *); void *arg; };
static void *task_entry(void *p) { struct task t = *(struct task *)p; free(p); t.fn(t.arg); return NULL; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    if (((!strcmp(name, "vibe_peripherals") || !strcmp(name, "vibe_battery")) && task_failure) ||
        ((!strcmp(name, "vibe_device") || !strcmp(name, "vibe_telemetry")) && sample_failure)) return 0;
    struct task *t = malloc(sizeof(*t)); assert(t); *t = (struct task){fn, arg};
    unsigned slot = thread_count++;
    assert(slot < 2 && pthread_create(&threads[slot], NULL, task_entry, t) == 0); return 1;
}
void vTaskDelete(void *task) { pthread_exit(NULL); }
void vTaskDelay(unsigned ms) { usleep(ms * 1000); if (atomic_load(&stopping)) pthread_exit(NULL); }
int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000 + atomic_load(&time_advance); }
esp_err_t esp_register_shutdown_handler(shutdown_handler_t handler) {
    assert(!shutdown_handler);
    if (registration_failure) return ESP_ERR_NO_MEM;
    shutdown_handler = handler; return ESP_OK;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) {
    assert(shutdown_handler);
    if (timer_failure) return ESP_ERR_NO_MEM;
    timer_callback = a->callback; *h = &motor_lock; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) {
    assert(h == &motor_lock && us > 0);
    atomic_store(&timer_active, true); atomic_fetch_add(&timer_start_count, 1); return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h) {
    assert(h == &motor_lock); atomic_store(&timer_active, false); return ESP_OK;
}
esp_err_t vibe_settings_snapshot(vibe_settings_snapshot_t *s) {
    assert(!telemetry);
    *s = (vibe_settings_snapshot_t){.brightness = 37, .haptics = true,
                                    .auto_rotate = atomic_load(&auto_rotate)};
    return 0;
}
esp_err_t factory_network_suspend(bool s) {
    assert(!telemetry);
    atomic_fetch_add(&network_calls, 1);
    if (atomic_exchange(&network_fail, false)) return ESP_FAIL;
    atomic_store(&network_sleeping, s); return ESP_OK;
}
bool tud_mounted(void) { return atomic_load(&usb_mounted); }
bool tud_suspended(void) { return atomic_load(&usb_suspended); }
esp_err_t bsp_motor_init(void) { assert(!telemetry); return sensor_failure ? ESP_FAIL : ESP_OK; }
esp_err_t bsp_motor_set(bool on) {
    assert(!sensor_failure);
    atomic_store(&motor_on, on);
    if (on) atomic_fetch_add(&motor_on_count, 1);
    return ESP_OK;
}
esp_err_t bsp_imu_init(void) { assert(!telemetry); return sensor_failure || (imu_retry && imu_attempts++ == 0) ? ESP_FAIL : ESP_OK; }
esp_err_t bsp_imu_deinit(void) { return ESP_OK; }
esp_err_t bsp_imu_start(const bsp_imu_config_t *c) { return 0; }
esp_err_t bsp_imu_get_accel(float *x, float *y, float *z) {
    if (atomic_load(&imu_read_failure)) return ESP_FAIL;
    *x = atomic_load(&sensor_orientation) == 90 ? 1 : 0; *y = 0; *z = *x ? 0 : 1; return 0;
}
esp_err_t vibe_battery_init(void) {
    atomic_store(&battery_entered, true);
    while (!atomic_load(&release_battery)) usleep(1000);
    atomic_store(&battery_initialized, !sensor_failure);
    return sensor_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t vibe_battery_read(vibe_battery_sample_t *s) {
    assert(atomic_load(&battery_initialized));
    *s = (vibe_battery_sample_t){.state_of_charge = atomic_load(&battery_soc),
        .current_ma = atomic_load(&battery_current), .voltage_mv = atomic_load(&battery_voltage),
        .status_flags = atomic_load(&battery_flags)};
    atomic_fetch_add(&battery_reads, 1);
    return atomic_load(&battery_error);
}
esp_err_t gpio_config(const gpio_config_t *c) { assert(!telemetry); return 0; }
int gpio_get_level(gpio_num_t pin) { assert(!telemetry); return pin == BSP_BUTTON_AI_GPIO ? atomic_load(&key_level) : 1; }
static esp_err_t rotate(unsigned degrees) {
    atomic_fetch_add(&rotation_calls, 1);
    return atomic_load(&rotation_failure) ? ESP_FAIL : ESP_OK;
}
static esp_err_t set_brightness(unsigned value) { atomic_store(&brightness, value); return 0; }
static void expect_key(unsigned key) {
    for (int i = 0; i < 200; ++i) {
        unsigned event = vibe_device_poll_key();
        if (event) { assert(event == key); return; }
        usleep(1000);
    }
    assert(!"key blocked by optional driver initialization");
}
static void expect_usb(bool connected) {
    for (unsigned i = 0; i < 200; ++i) {
        vibe_device_snapshot_t snapshot;
        assert(vibe_device_snapshot(&snapshot) == ESP_OK);
        if (snapshot.usb_connected == connected) return;
        usleep(1000);
    }
    assert(!"USB snapshot did not follow the live bus state");
}
static void expect_orientation(unsigned orientation) {
    for (unsigned i = 0; i < 1500; ++i) {
        vibe_device_snapshot_t value; vibe_device_snapshot(&value);
        if (value.orientation == orientation) return;
        usleep(1000);
    }
    assert(!"orientation did not follow policy");
}
static void expect_telemetry(bool usb, bool known, unsigned percent, bool charging)
{
    const unsigned before = atomic_load(&battery_reads);
    atomic_fetch_add(&time_advance, 3000000);
    for (unsigned i = 0; i < 1000; ++i) {
        vibe_device_snapshot_t v; assert(vibe_device_snapshot(&v) == ESP_OK);
        if (atomic_load(&battery_reads) > before && v.usb_connected == usb && v.battery_valid == known &&
            v.battery_percent == percent && v.charging == charging) return;
        usleep(1000);
    }
    assert(!"telemetry transition was not published");
}
static void check_telemetry(const char *scenario)
{
    telemetry = true;
    sensor_failure = !strcmp(scenario, "telemetry-battery-failure");
    task_failure = !strcmp(scenario, "telemetry-task-failure");
    if (!strcmp(scenario, "telemetry-retry")) {
        sample_failure = true;
        assert(vibe_device_start_telemetry() == ESP_ERR_NO_MEM);
        sample_failure = false;
    }
    assert(vibe_device_start_telemetry() == ESP_OK);
    assert(vibe_device_start_telemetry() == ESP_OK);
    assert(vibe_device_start(rotate, set_brightness) == ESP_ERR_INVALID_STATE);
    vibe_device_policy(true, true);
    vibe_device_haptic(5);
    assert(vibe_device_poll_key() == 0);
    vibe_device_snapshot_t snapshot;
    for (unsigned i = 0; i < 500; ++i) {
        assert(vibe_device_snapshot(&snapshot) == ESP_OK);
        if (snapshot.usb_connected) break;
        usleep(1000);
    }
    assert(snapshot.usb_connected && !snapshot.battery_valid);
    if (!task_failure) {
        for (unsigned i = 0; i < 500 && !atomic_load(&battery_entered); ++i) usleep(1000);
        assert(atomic_load(&battery_entered));
        assert(!snapshot.peripherals_ready);
    }
    atomic_store(&release_battery, true);
    if (!task_failure) pthread_join(threads[1], NULL);
    for (unsigned i = 0; i < 1000; ++i) {
        vibe_device_snapshot(&snapshot);
        if (snapshot.peripherals_ready) break;
        usleep(1000);
    }
    assert(snapshot.peripherals_ready && snapshot.usb_connected);
    assert(snapshot.battery_valid == (!task_failure && !sensor_failure));
    if (snapshot.battery_valid) assert(snapshot.battery_percent == 73 && snapshot.charging);
    assert(!snapshot.haptics_available && !snapshot.imu_available);
    assert(snapshot.orientation == 0 && snapshot.motion_revision == 0);
    assert(!atomic_load(&brightness) && !atomic_load(&network_calls));
    assert(!shutdown_handler && !timer_callback && !atomic_load(&motor_on));
    if (!strcmp(scenario, "telemetry-transitions")) {
        atomic_store(&usb_suspended, true); expect_telemetry(false, true, 73, true);
        atomic_store(&usb_mounted, false); atomic_store(&usb_suspended, false);
        expect_telemetry(false, true, 73, true);
        atomic_store(&usb_mounted, true); expect_telemetry(true, true, 73, true);
        atomic_store(&battery_current, 0); atomic_store(&battery_soc, 60);
        expect_telemetry(true, true, 60, false);
        atomic_store(&battery_current, -15); atomic_store(&battery_soc, 0);
        expect_telemetry(true, true, 0, false);
        atomic_store(&battery_soc, 101); expect_telemetry(true, false, 0, false);
        atomic_store(&battery_soc, 80); atomic_store(&battery_flags, 0);
        expect_telemetry(true, false, 0, false);
        atomic_store(&battery_flags, 8); atomic_store(&battery_error, ESP_FAIL);
        expect_telemetry(true, false, 0, false);
        atomic_store(&battery_error, ESP_OK); atomic_store(&battery_voltage, 0);
        expect_telemetry(true, false, 0, false);
        atomic_store(&battery_voltage, 4000); atomic_store(&battery_current, 20);
        expect_telemetry(true, true, 80, true);
    }
    atomic_store(&stopping, true);
    pthread_join(threads[0], NULL);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strncmp(argv[1], "telemetry", 9)) { check_telemetry(argv[1]); return 0; }
    sensor_failure = !strcmp(argv[1], "sensor-failure");
    imu_retry = !strcmp(argv[1], "imu-retry");
    task_failure = !strcmp(argv[1], "task-failure");
    registration_failure = !strcmp(argv[1], "shutdown-registration-failure");
    timer_failure = !strcmp(argv[1], "motor-timer-failure");
    if (!strcmp(argv[1], "retry")) {
        sample_failure = true;
        assert(vibe_device_start(rotate, set_brightness) == ESP_ERR_NO_MEM);
        sample_failure = false;
    }
    assert(vibe_device_start(rotate, set_brightness) == ESP_OK);
    assert(vibe_device_start(rotate, set_brightness) == ESP_OK);
    assert(vibe_device_start_telemetry() == ESP_ERR_INVALID_STATE);
    if (!task_failure) {
        for (int i = 0; i < 500 && !atomic_load(&battery_entered); ++i) usleep(1000);
        assert(atomic_load(&battery_entered));
    }
    atomic_store(&key_level, 0); expect_key(1);
    atomic_store(&key_level, 1); expect_key(2);
    assert(atomic_load(&brightness) == 37);
    vibe_device_snapshot_t s; vibe_device_snapshot(&s);
    assert(s.usb_connected && !s.battery_valid);
    if (!strcmp(argv[1], "usb-lifecycle")) {
        for (unsigned i = 0; i < 3; ++i) {
            /* Battery-backed unplug can suspend the bus without unmounting. */
            atomic_store(&usb_suspended, true);
            expect_usb(false);
            atomic_store(&usb_mounted, false);
            atomic_store(&usb_suspended, false);
            expect_usb(false);
            atomic_store(&usb_mounted, true);
            expect_usb(true);
        }
        /* Keys remain responsive while USB is suspended and sensors start. */
        atomic_store(&usb_suspended, true); expect_usb(false);
        atomic_store(&key_level, 0); expect_key(1);
        atomic_store(&key_level, 1); expect_key(2);
        atomic_store(&usb_suspended, false); expect_usb(true);
    }
    bool sleep_retry = !strcmp(argv[1], "wifi-sleep-retry"),
         wake_retry = !strcmp(argv[1], "wifi-wake-retry");
    atomic_store(&network_fail, sleep_retry);
    vibe_device_policy(true, false);
    for (int i = 0; i < 200 && atomic_load(&brightness) != 10; ++i) usleep(1000);
    assert(atomic_load(&brightness) == 10);
    if (sleep_retry || wake_retry) {
        for (int i = 0; i < 1600 && !atomic_load(&network_sleeping); ++i) usleep(1000);
        assert(atomic_load(&network_sleeping));
        unsigned calls = atomic_load(&network_calls);
        assert(calls == (sleep_retry ? 2U : 1U));
        if (wake_retry) {
            atomic_store(&network_fail, true);
            vibe_device_policy(false, false);
            for (int i = 0; i < 2500 && atomic_load(&network_sleeping); ++i) usleep(1000);
            assert(!atomic_load(&network_sleeping) && atomic_load(&network_calls) == calls + 2);
        }
        atomic_store(&key_level, 0); expect_key(1);
        atomic_store(&key_level, 1); expect_key(2);
    }
    atomic_store(&release_battery, true);
    if (!task_failure) pthread_join(threads[1], NULL);
    for (int i = 0; i < 200; ++i) {
        vibe_device_snapshot(&s);
        if (s.battery_valid && s.peripherals_ready) break;
        usleep(1000);
    }
    assert(s.battery_valid == (!task_failure && !sensor_failure));
    assert(s.peripherals_ready);
    if (imu_retry) assert(imu_attempts == 2 && s.imu_available);
    if (s.battery_valid) {
        assert(s.battery_percent == 73 && s.charging && s.imu_available);
        assert(s.haptics_available == (!registration_failure && !timer_failure));
    }
    else assert(!s.imu_available && !s.haptics_available);
    if (!strcmp(argv[1], "rotation-retry") || !strcmp(argv[1], "rotation-off-after-read-error") ||
        !strcmp(argv[1], "rotation-imu-failure")) {
        vibe_device_policy(false, true);
        atomic_store(&sensor_orientation, 90); atomic_store(&auto_rotate, true);
        bool retry = !strcmp(argv[1], "rotation-retry");
        atomic_store(&rotation_failure, retry);
        if (retry) {
            for (unsigned i = 0; i < 1000 && !atomic_load(&rotation_calls); ++i) usleep(1000);
            assert(atomic_load(&rotation_calls));
            vibe_device_snapshot(&s); assert(s.orientation == 0);
            atomic_store(&rotation_failure, false);
        }
        expect_orientation(90);
        atomic_store(&imu_read_failure, !retry); atomic_store(&auto_rotate, false);
        expect_orientation(0);
        atomic_store(&key_level, 0); expect_key(1);
        atomic_store(&key_level, 1); expect_key(2);
    }
    assert((shutdown_handler != NULL) == (!sensor_failure && !task_failure && !registration_failure));
    if (!strcmp(argv[1], "shutdown-active") || !strcmp(argv[1], "shutdown-paused")) {
        vibe_device_haptic(5);
        for (int i = 0; i < 200 && (!atomic_load(&motor_on) || !atomic_load(&timer_active)); ++i) usleep(1000);
        assert(atomic_load(&motor_on) && atomic_load(&timer_active));
        if (!strcmp(argv[1], "shutdown-paused")) {
            timer_callback(NULL); /* Pause before the second pulse. */
            assert(!atomic_load(&motor_on) && atomic_load(&timer_active));
        }
        vibe_device_haptic(2); /* A worker may already have copied this request. */
    }
    if (shutdown_handler) {
        shutdown_handler();
        assert(!atomic_load(&motor_on) && !atomic_load(&timer_active));
        const unsigned starts = atomic_load(&timer_start_count), ons = atomic_load(&motor_on_count);
        if (timer_callback) timer_callback(NULL); /* Expired callback dispatched after stop. */
        vibe_device_haptic(5); /* UI input arriving during shutdown must stay silent. */
        usleep(80000);
        assert(!atomic_load(&motor_on) && !atomic_load(&timer_active));
        assert(atomic_load(&motor_on_count) == ons && atomic_load(&timer_start_count) == starts);
        shutdown_handler(); /* Cleanup is also safe after partial initialization. */
    } else {
        vibe_device_haptic(5); usleep(80000);
        assert(!atomic_load(&motor_on) && !atomic_load(&timer_active));
    }
    atomic_store(&stopping, true); pthread_join(threads[0], NULL);
    return 0;
}
