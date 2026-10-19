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
#include "vibe_input_queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "vibe_ui";
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static vibe_input_queue_t s_input;
static bool s_input_down[2]; /* USB, physical touch */
static SemaphoreHandle_t s_display_ready;
static esp_display_present_target_config_t s_display;
static esp_err_t s_display_error;
static struct {
    vui_handle_t ui;
    esp_display_presenter_t *presenter;
    esp_lcd_touch_handle_t touch;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    uint16_t *frame;
} s_device;

static void display_prepare_task(void *unused)
{
    (void)unused;
    const int64_t started = esp_timer_get_time();
    s_display_error = board_display_init(&s_display);
    ESP_LOGI("boot_perf", "display_init_us=%lld result=%d stack_free=%u",
             (long long)(esp_timer_get_time() - started), (int)s_display_error,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_display_ready);
    vTaskDelete(NULL);
}

esp_err_t factory_ui_prepare(void)
{
    if (s_device.ui) return ESP_ERR_INVALID_STATE;
    if (s_display_ready) return ESP_OK;
    s_display_ready = xSemaphoreCreateBinary();
    if (!s_display_ready) return ESP_ERR_NO_MEM;
    if (xTaskCreate(display_prepare_task, "vibe_display", 6144, NULL, 5, NULL) != pdPASS) {
        vSemaphoreDelete(s_display_ready);
        s_display_ready = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool enqueue_input(int16_t x, int16_t y, bool pressed, unsigned source)
{
    vibe_input_t event = {.x = x, .y = y, .source = source,
                          .time_ms = (uint64_t)esp_timer_get_time() / 1000};
    taskENTER_CRITICAL(&s_input_lock);
    event.kind = pressed ? (s_input_down[source] ? VIBE_INPUT_MOVE : VIBE_INPUT_DOWN)
                         : VIBE_INPUT_UP;
    bool accepted = vibe_input_push(&s_input, event);
    if (accepted) s_input_down[source] = pressed;
    taskEXIT_CRITICAL(&s_input_lock);
    if (accepted) xTaskNotifyGive(s_device.task);
    return accepted;
}
static bool dequeue_input(vibe_input_t *event)
{
    taskENTER_CRITICAL(&s_input_lock);
    bool available = vibe_input_pop(&s_input, event);
    taskEXIT_CRITICAL(&s_input_lock);
    return available;
}
static void touch_task(void *unused)
{
    (void)unused;
    bool touching = false;
    int16_t last_x = 0, last_y = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (esp_lcd_touch_read_data(s_device.touch) == ESP_OK) {
            esp_lcd_touch_point_data_t point = {0};
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(s_device.touch, &point, &count, 1) == ESP_OK) {
                bool pressed = count != 0;
                int16_t x = pressed ? point.x : last_x, y = pressed ? point.y : last_y;
                if ((pressed != touching || (pressed && (x != last_x || y != last_y))) &&
                    enqueue_input(x, y, pressed, 1)) {
                    touching = pressed; last_x = x; last_y = y;
                }
            }
        }
        /* Sampling never takes the render/display mutex. The UI task maps
         * physical coordinates and is the only caller of the Vui API. */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}

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
    taskENTER_CRITICAL(&s_input_lock);
    bool active = s_input.count || s_input_down[0] || s_input_down[1];
    taskEXIT_CRITICAL(&s_input_lock);
    if (active) {
        xSemaphoreGiveRecursive(s_device.lock);
        return ESP_ERR_INVALID_STATE;
    }
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
static esp_err_t present_frame(const vui_area_t *changed)
{
    /* Keep a single presenter producer; rotation also holds s_device.lock. */
    esp_err_t err = esp_display_presenter_rebind_producer(s_device.presenter);
    if (err != ESP_OK) return err;
    const esp_display_present_area_t dirty = {
        changed->x, changed->y, changed->x + changed->w - 1, changed->y + changed->h - 1};
    const esp_display_present_surface_request_t request = {
        .dirty_areas = &dirty, .dirty_area_count = 1};
    esp_display_present_area_t areas[1];
    size_t count = 0;
    bool full = false;
    err = esp_display_presenter_begin_next_frame(s_device.presenter, &request, areas, 1,
                                                 &count, &full);
    if (err != ESP_OK) return err;
    /* A fresh/rotated transport may require full coverage even for small
     * damage. The retained framebuffer always contains the complete image. */
    if (full) { areas[0] = (esp_display_present_area_t){0, 0, 479, 479}; count = 1; }
    if (!count) err = ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        const esp_display_present_area_t *area = &areas[i];
        size_t stride = (area->x2 - area->x1 + 1) * sizeof(uint16_t);
        for (int y = area->y1; y <= area->y2;) {
            esp_display_presenter_buffer_t buffer;
            err = esp_display_presenter_acquire_buffer(s_device.presenter, &buffer);
            if (err != ESP_OK) break;
            size_t rows = area->y2 - y + 1;
            if (buffer.resolve_rows)
                err = buffer.resolve_rows(buffer.resolve_rows_ctx, buffer.lease_id, stride, rows, &rows);
            else if (rows > buffer.capacity_bytes / stride)
                rows = buffer.capacity_bytes / stride;
            if (err != ESP_OK || !rows || rows > (size_t)(area->y2 - y + 1) ||
                rows > buffer.capacity_bytes / stride) {
                if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (stride == 960)
                memcpy(buffer.surface.pixels, s_device.frame + y * 480, rows * stride);
            else
                for (size_t row = 0; row < rows; ++row)
                    memcpy((uint8_t *)buffer.surface.pixels + row * stride,
                           s_device.frame + (y + row) * 480 + area->x1, stride);
            esp_display_present_area_t part = {area->x1, y, area->x2, y + rows - 1};
            err = esp_display_presenter_submit_buffer(s_device.presenter, &buffer, &part, stride);
            if (err != ESP_OK) break;
            y += rows;
        }
    }
    if (err == ESP_OK) {
        const esp_display_presenter_submit_t submit = {
            .coverage = full ? ESP_DISPLAY_PRESENT_COVERAGE_FULL : ESP_DISPLAY_PRESENT_COVERAGE_AREAS,
            .areas = full ? NULL : areas, .area_count = full ? 0 : count};
        err = esp_display_presenter_commit_frame(s_device.presenter, &submit);
    }
    if (err != ESP_OK) esp_display_presenter_cancel_frame(s_device.presenter);
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
        const uint32_t elapsed = (now - previous) / 1000;
        vui_tick(s_device.ui, elapsed);
        previous += (int64_t)elapsed * 1000; /* Keep the fractional millisecond. */
        vibe_input_t input;
        for (unsigned n = 0; n < VIBE_INPUT_CAPACITY && dequeue_input(&input); ++n) {
            int32_t x = input.x, y = input.y;
            if (input.source && esp_display_presenter_map_point(s_device.presenter, &x, &y) != ESP_OK)
                continue;
            vui_pointer_at(s_device.ui, x, y, input.kind != VIBE_INPUT_UP, input.time_ms);
        }
        vui_area_t changed;
        if (vui_render_changed(s_device.ui, s_device.frame, &changed)) {
            esp_err_t err = present_frame(&changed);
            static bool first_frame;
            if (err == ESP_OK && !first_frame) {
                first_frame = true;
                ESP_LOGI("boot_perf", "first_frame_submitted_at_us=%lld",
                         (long long)esp_timer_get_time());
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "display frame: %s", esp_err_to_name(err));
                vui_invalidate(s_device.ui);
            }
        }
        xSemaphoreGiveRecursive(s_device.lock);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}
esp_err_t factory_ui_start(void)
{
    if (s_device.ui)
        return ESP_ERR_INVALID_STATE;
    s_device.lock = xSemaphoreCreateRecursiveMutex();
    s_device.frame =
        heap_caps_aligned_alloc(64, 480 * 480 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_device.lock || !s_device.frame)
        return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(factory_ui_prepare(), TAG, "prepare display");
    /* Inflate UI assets while the panel is still settling. No renderer or
     * screen/input provider can access the UI until the handoff below. */
    const int64_t assets_started = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(vui_create(&s_device.ui), TAG, "UI assets");
    ESP_LOGI("boot_perf", "ui_assets_us=%lld", (long long)(esp_timer_get_time() - assets_started));
    if (xSemaphoreTake(s_display_ready, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    /* The worker no longer touches panel state after signalling. */
    ESP_RETURN_ON_ERROR(s_display_error, TAG, "display");
    ESP_RETURN_ON_ERROR(board_touch_init(&s_device.touch), TAG, "touch");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen backend");
    esp_display_presenter_config_t config = {.width = 480,
                                             .height = 480,
                                             .pixel_format =
                                                 ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
                                             .max_damage_areas = 1,
                                             .target = s_display};
    ESP_RETURN_ON_ERROR(esp_display_presenter_create(&config, &s_device.presenter), TAG,
                        "presenter");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(s_device.ui), TAG, "screen attach");
    /* Initialize before task handoff; all later controller and renderer calls
     * run serially on the render task, including queued Iris requests. */
    ESP_RETURN_ON_ERROR(factory_ui_dispatch_start(s_device.ui, factory_ui_services()), TAG,
                        "controller");
    if (xTaskCreate(render_task, "vibe_ui", 8192, NULL, 5, &s_device.task) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (s_device.touch && xTaskCreate(touch_task, "vibe_touch", 3072, NULL, 6, NULL) != pdPASS)
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
    return enqueue_input(x, y, pressed, 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}
