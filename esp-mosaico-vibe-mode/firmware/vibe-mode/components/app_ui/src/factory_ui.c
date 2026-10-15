// SPDX-License-Identifier: Apache-2.0
#include "factory_ui.h"
#include "factory_ui_dispatch.h"
#include "vibe_ui.h"
#include "board_display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "iris_screen_mirror.h"
#include "ui_services.h"
#include "bsp/esp_mosaico.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "vibe_ui";
typedef struct {
    int16_t x, y;
    bool pressed;
} input_t;
static struct {
    vui_handle_t ui;
    esp_display_presenter_t *presenter;
    esp_lcd_touch_handle_t touch;
    SemaphoreHandle_t lock;
    QueueHandle_t input;
    TaskHandle_t task;
    uint16_t *frame;
    bool touching;
    int16_t last_x, last_y;
} s_device;

esp_err_t factory_ui_set_brightness(unsigned percent)
{
    if (percent > 100)
        return ESP_ERR_INVALID_ARG;
    if (!s_device.ui)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_device.lock, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    /* The LCD command shares the frame-transfer SPI device. A mutex alone
     * cannot protect transfers still pending after present_frame() returns. */
    esp_err_t err = esp_display_presenter_quiesce(s_device.presenter, 500);
    if (err == ESP_OK)
        err = bsp_display_brightness_set(percent);
    xSemaphoreGiveRecursive(s_device.lock);
    return err;
}

esp_err_t factory_ui_rotate(unsigned degrees)
{
    if (!s_device.ui || (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270))
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(s_device.lock, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = esp_display_presenter_quiesce(s_device.presenter, 500);
    /* Rotation stays inside the presenter so transfer and touch transforms
     * share one orientation. Do not additionally rotate the physical panel. */
    if (err == ESP_OK)
        err = esp_display_presenter_set_rotation(s_device.presenter,
                                                 (esp_display_present_rotation_t)degrees);
    if (err == ESP_OK)
        vui_invalidate(s_device.ui);
    xSemaphoreGiveRecursive(s_device.lock);
    return err;
}
static esp_err_t present_frame(void)
{
    /* Rotation can rebuild transport ownership on the policy task while
     * this render task is quiescent. Reclaim it before submitting a frame. */
    esp_err_t bound = esp_display_presenter_rebind_producer(s_device.presenter);
    if (bound != ESP_OK)
        return bound;
    esp_display_present_area_t areas[1];
    size_t count = 0;
    bool full = false;
    esp_err_t err =
        esp_display_presenter_begin_next_frame(s_device.presenter, NULL, areas, 1, &count, &full);
    if (err != ESP_OK)
        return err;
    for (unsigned y = 0; y < 480;) {
        esp_display_presenter_buffer_t buffer;
        err = esp_display_presenter_acquire_buffer(s_device.presenter, &buffer);
        if (err != ESP_OK)
            break;
        size_t rows = 480 - y;
        if (buffer.resolve_rows)
            err = buffer.resolve_rows(buffer.resolve_rows_ctx, buffer.lease_id, 960, rows, &rows);
        else if (rows > buffer.capacity_bytes / 960)
            rows = buffer.capacity_bytes / 960;
        if (err != ESP_OK || !rows || rows > 480 - y || rows > buffer.capacity_bytes / 960) {
            if (err == ESP_OK)
                err = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(buffer.surface.pixels, s_device.frame + y * 480, rows * 960);
        esp_display_present_area_t area = {0, y, 479, y + rows - 1};
        err = esp_display_presenter_submit_buffer(s_device.presenter, &buffer, &area, 960);
        if (err != ESP_OK)
            break;
        y += rows;
    }
    if (err == ESP_OK) {
        const esp_display_presenter_submit_t submit = {.coverage =
                                                           ESP_DISPLAY_PRESENT_COVERAGE_FULL};
        err = esp_display_presenter_commit_frame(s_device.presenter, &submit);
    }
    if (err != ESP_OK)
        esp_display_presenter_cancel_frame(s_device.presenter);
    return err;
}
static void render_task(void *unused)
{
    (void)unused;
    ESP_ERROR_CHECK(esp_display_presenter_rebind_producer(s_device.presenter));
    int64_t previous = esp_timer_get_time();
    for (;;) {
        xSemaphoreTakeRecursive(s_device.lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        vui_tick(s_device.ui, (now - previous) / 1000);
        previous = now;
        input_t input;
        while (xQueueReceive(s_device.input, &input, 0) == pdTRUE)
            vui_pointer(s_device.ui, input.x, input.y, input.pressed);
        if (s_device.touch && esp_lcd_touch_read_data(s_device.touch) == ESP_OK) {
            esp_lcd_touch_point_data_t point = {0};
            uint8_t count = 0;
            bool pressed =
                esp_lcd_touch_get_data(s_device.touch, &point, &count, 1) == ESP_OK && count;
            uint16_t x = point.x, y = point.y;
            if (pressed) {
                int32_t px = x, py = y;
                if (esp_display_presenter_map_point(s_device.presenter, &px, &py) == ESP_OK) {
                    s_device.last_x = px;
                    s_device.last_y = py;
                    vui_pointer(s_device.ui, px, py, true);
                }
            } else if (s_device.touching)
                vui_pointer(s_device.ui, s_device.last_x, s_device.last_y, false);
            s_device.touching = pressed;
        }
        if (vui_render(s_device.ui, s_device.frame)) {
            esp_err_t err = present_frame();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "display frame: %s", esp_err_to_name(err));
                vui_invalidate(s_device.ui);
            }
        }
        xSemaphoreGiveRecursive(s_device.lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
esp_err_t factory_ui_start(void)
{
    if (s_device.ui)
        return ESP_ERR_INVALID_STATE;
    s_device.lock = xSemaphoreCreateRecursiveMutex();
    s_device.input = xQueueCreate(16, sizeof(input_t));
    s_device.frame =
        heap_caps_aligned_alloc(64, 480 * 480 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_device.lock || !s_device.input || !s_device.frame)
        return ESP_ERR_NO_MEM;
    esp_display_present_target_config_t display;
    ESP_RETURN_ON_ERROR(board_display_init(&display), TAG, "display");
    ESP_RETURN_ON_ERROR(board_touch_init(&s_device.touch), TAG, "touch");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen backend");
    esp_display_presenter_config_t config = {.width = 480,
                                             .height = 480,
                                             .pixel_format =
                                                 ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
                                             .max_damage_areas = 1,
                                             .target = display};
    ESP_RETURN_ON_ERROR(esp_display_presenter_create(&config, &s_device.presenter), TAG,
                        "presenter");
    ESP_RETURN_ON_ERROR(vui_create(&s_device.ui), TAG, "UI assets");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(s_device.ui), TAG, "screen attach");
    /* Initialize before task handoff; all later controller and renderer calls
     * run serially on the render task, including queued Iris requests. */
    ESP_RETURN_ON_ERROR(factory_ui_dispatch_start(s_device.ui, factory_ui_services()), TAG,
                        "controller");
    if (xTaskCreate(render_task, "vibe_ui", 8192, NULL, 5, &s_device.task) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Vibe lightweight UI ready at 480x480");
    return ESP_OK;
}
esp_err_t factory_ui_open_bridge(void *ctx)
{
    (void)ctx;
    return factory_ui_dispatch_open_bridge();
}
esp_err_t factory_ui_inject_touch(void *ctx, int16_t x, int16_t y, bool pressed)
{
    (void)ctx;
    if (!s_device.ui || !s_device.task)
        return ESP_ERR_INVALID_STATE;
    if (x < 0 || x >= 480 || y < 0 || y >= 480)
        return ESP_ERR_INVALID_ARG;
    input_t input = {x, y, pressed};
    return xQueueSend(s_device.input, &input, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK
                                                                            : ESP_ERR_TIMEOUT;
}
