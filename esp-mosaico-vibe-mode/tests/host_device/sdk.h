// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef void (*shutdown_handler_t)(void);
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(x) pthread_mutex_lock(x)
#define taskEXIT_CRITICAL(x) pthread_mutex_unlock(x)
typedef struct test_queue *QueueHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { unsigned brightness; bool haptics, auto_rotate; } vibe_settings_snapshot_t;
typedef struct { unsigned state_of_charge; int current_ma; } bsp_battery_status_t;
typedef struct { int unused; } bsp_imu_config_t;
#define BSP_IMU_CONFIG_DEFAULT() {0}
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
#define BSP_BUTTON_AI_GPIO 1
#define BSP_BUTTON_BOOT_GPIO 2
#define BSP_BUTTON_ACTIVE_LEVEL 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
int xQueueSend(QueueHandle_t, const void *, unsigned);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void xSemaphoreGive(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
void vTaskDelay(unsigned);
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_register_shutdown_handler(shutdown_handler_t);
esp_err_t vibe_settings_snapshot(vibe_settings_snapshot_t *);
esp_err_t factory_network_suspend(bool);
bool tud_mounted(void);
esp_err_t bsp_motor_init(void);
esp_err_t bsp_motor_set(bool);
esp_err_t bsp_imu_init(void);
esp_err_t bsp_imu_start(const bsp_imu_config_t *);
esp_err_t bsp_imu_get_accel(float *, float *, float *);
esp_err_t bsp_battery_init(void);
esp_err_t bsp_battery_read(bsp_battery_status_t *);
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(gpio_num_t);
