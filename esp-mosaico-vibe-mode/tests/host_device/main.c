// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include "vibe_device.h"
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
static atomic_bool motor_on, timer_active;
static atomic_uint motor_on_count, timer_start_count;
static shutdown_handler_t shutdown_handler;
static void (*timer_callback)(void *);

QueueHandle_t xQueueCreate(unsigned n, unsigned size) { assert(n == 16 && size == sizeof(unsigned)); return &keys; }
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
    if ((!strcmp(name, "vibe_peripherals") && task_failure) ||
        (!strcmp(name, "vibe_device") && sample_failure)) return 0;
    struct task *t = malloc(sizeof(*t)); assert(t); *t = (struct task){fn, arg};
    unsigned slot = thread_count++;
    assert(slot < 2 && pthread_create(&threads[slot], NULL, task_entry, t) == 0); return 1;
}
void vTaskDelete(void *task) { pthread_exit(NULL); }
void vTaskDelay(unsigned ms) { usleep(ms * 1000); if (atomic_load(&stopping)) pthread_exit(NULL); }
int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
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
esp_err_t vibe_settings_snapshot(vibe_settings_snapshot_t *s) { *s = (vibe_settings_snapshot_t){.brightness = 37, .haptics = true}; return 0; }
esp_err_t factory_network_suspend(bool s) {
    atomic_fetch_add(&network_calls, 1);
    if (atomic_exchange(&network_fail, false)) return ESP_FAIL;
    atomic_store(&network_sleeping, s); return ESP_OK;
}
bool tud_mounted(void) { return true; }
esp_err_t bsp_motor_init(void) { return sensor_failure ? ESP_FAIL : ESP_OK; }
esp_err_t bsp_motor_set(bool on) {
    assert(!sensor_failure);
    atomic_store(&motor_on, on);
    if (on) atomic_fetch_add(&motor_on_count, 1);
    return ESP_OK;
}
esp_err_t bsp_imu_init(void) { return sensor_failure ? ESP_FAIL : ESP_OK; }
esp_err_t bsp_imu_start(const bsp_imu_config_t *c) { return 0; }
esp_err_t bsp_imu_get_accel(float *x, float *y, float *z) { *x = *y = 0; *z = 1; return 0; }
esp_err_t bsp_battery_init(void) {
    atomic_store(&battery_entered, true);
    while (!atomic_load(&release_battery)) usleep(1000);
    atomic_store(&battery_initialized, !sensor_failure);
    return sensor_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_battery_read(bsp_battery_status_t *s) {
    assert(atomic_load(&battery_initialized)); *s = (bsp_battery_status_t){73, 10}; return 0;
}
esp_err_t gpio_config(const gpio_config_t *c) { return 0; }
int gpio_get_level(gpio_num_t pin) { return pin == BSP_BUTTON_AI_GPIO ? atomic_load(&key_level) : 1; }
static esp_err_t rotate(unsigned degrees) { return 0; }
static esp_err_t set_brightness(unsigned value) { atomic_store(&brightness, value); return 0; }
static void expect_key(unsigned key) {
    for (int i = 0; i < 200; ++i) {
        unsigned event = vibe_device_poll_key();
        if (event) { assert(event == key); return; }
        usleep(1000);
    }
    assert(!"key blocked by optional driver initialization");
}
int main(int argc, char **argv) {
    assert(argc == 2);
    sensor_failure = !strcmp(argv[1], "sensor-failure");
    task_failure = !strcmp(argv[1], "task-failure");
    registration_failure = !strcmp(argv[1], "shutdown-registration-failure");
    timer_failure = !strcmp(argv[1], "motor-timer-failure");
    if (!strcmp(argv[1], "retry")) {
        sample_failure = true;
        assert(vibe_device_start(rotate, set_brightness) == ESP_ERR_NO_MEM);
        sample_failure = false;
    }
    assert(vibe_device_start(rotate, set_brightness) == ESP_OK);
    if (!task_failure) {
        for (int i = 0; i < 500 && !atomic_load(&battery_entered); ++i) usleep(1000);
        assert(atomic_load(&battery_entered));
    }
    atomic_store(&key_level, 0); expect_key(1);
    atomic_store(&key_level, 1); expect_key(2);
    assert(atomic_load(&brightness) == 37);
    vibe_device_snapshot_t s; vibe_device_snapshot(&s);
    assert(s.usb_connected && !s.battery_valid);
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
        if (s.battery_valid) break;
        usleep(1000);
    }
    assert(s.battery_valid == (!task_failure && !sensor_failure));
    if (s.battery_valid) {
        assert(s.battery_percent == 73 && s.charging && s.imu_available);
        assert(s.haptics_available == (!registration_failure && !timer_failure));
    }
    else assert(!s.imu_available && !s.haptics_available);
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
