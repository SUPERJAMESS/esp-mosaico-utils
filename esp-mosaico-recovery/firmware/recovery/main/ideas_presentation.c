// SPDX-License-Identifier: Apache-2.0
#include "ideas_presentation.h"
#include "ideas_package.h"
#include "esp_gsp_deployable.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>

typedef struct {
    atomic_bool active, enabled, closed, pressed, deciding;
    atomic_int target;
    esp_gsp_deployable_bundle_t *bundle;
    esp_gsp_esp_lcd_session_t *session;
    esp_gsp_handle_t app, hub;
    ideas_presentation_options_t options;
    esp_err_t prepare_error;
} card_t;
static card_t s_card;

static int hit(int32_t x, int32_t y)
{
    if (x >= 0 && x < 72 && y >= 0 && y < 72) return 1;
    if (!s_card.options.decide || y < 408 || y >= 464) return 0;
    if (x >= 24 && x < 228) return 1;
    if (x >= 252 && x < 456) return 2;
    return 0;
}
bool ideas_presentation_input(int32_t x, int32_t y, bool pressed)
{
    if (!atomic_load(&s_card.active)) return false;
    if (!atomic_load(&s_card.enabled)) return true;
    bool was_pressed = atomic_exchange(&s_card.pressed, pressed);
    if (pressed && !was_pressed) atomic_store(&s_card.target, hit(x, y));
    if (!pressed && was_pressed) {
        int target = atomic_exchange(&s_card.target, 0);
        if (target && target == hit(x, y) && !atomic_exchange(&s_card.deciding, true)) {
            if (!s_card.options.decide) atomic_store(&s_card.closed, true);
            else if (s_card.options.decide(s_card.options.request_id, target == 2) != ESP_OK)
                atomic_store(&s_card.deciding, false);
        }
    }
    return true;
}
static bool input(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui; (void)ctx;
    (void)ideas_presentation_input(x, y, pressed);
    /* Downloaded CALL actions are never dispatched. */
    return true;
}
static uint32_t key(const char *name)
{
    uint32_t value = 2166136261U;
    for (; *name; ++name) value = (value ^ (uint8_t)*name) * 16777619U;
    return value;
}
static void prepare(esp_gsp_handle_t ui, void *ctx)
{
    card_t *card = ctx;
    esp_err_t err = esp_gsp_set_input_interceptor(ui, input, ctx);
    if (err == ESP_OK && card->options.decide) {
        err = esp_gsp_component_set_text(ui, key("idea_download_summary"), card->options.summary);
        if (err == ESP_OK) err = esp_gsp_component_set_text(ui, key("idea_download_accept"),
            card->options.already_downloaded ? "My Ideas" : "Download");
        if (err == ESP_OK) err = esp_gsp_component_set_visible(ui, key("idea_download_controls"), true);
    }
    card->prepare_error = err;
}
void ideas_presentation_enable(void) { atomic_store(&s_card.enabled, true); }
void ideas_presentation_close(void)
{
    if (!atomic_load(&s_card.active)) return;
    atomic_store(&s_card.enabled, false);
    if (s_card.session && s_card.app) {
        while (esp_gsp_esp_lcd_resume(s_card.session, s_card.app, &s_card.hub) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_card.session = NULL; s_card.app = NULL;
    atomic_store(&s_card.active, false);
    if (s_card.bundle) esp_gsp_deployable_bundle_close(s_card.bundle);
    s_card.bundle = NULL;
}
esp_err_t ideas_presentation_open(esp_gsp_handle_t hub, const void *data, size_t size,
                                  const ideas_presentation_options_t *options)
{
    if (!hub || !data || !size || size > IDEAS_MAX_ASSET_BYTES) return ESP_ERR_INVALID_SIZE;
    if (atomic_load(&s_card.active)) return ESP_ERR_INVALID_STATE;
    s_card.options = options ? *options : (ideas_presentation_options_t){0};
    s_card.prepare_error = ESP_OK;
    s_card.hub = hub; s_card.app = NULL; s_card.session = NULL;
    atomic_store(&s_card.closed, false); atomic_store(&s_card.pressed, false);
    atomic_store(&s_card.deciding, false); atomic_store(&s_card.enabled, false);
    atomic_store(&s_card.target, 0);
    esp_err_t err = esp_gsp_deployable_bundle_open(data, size, true, &s_card.bundle);
    esp_gsp_deployable_info_t info = {.struct_size = sizeof(info)};
    if (err == ESP_OK) err = esp_gsp_deployable_bundle_get_info(s_card.bundle, &info);
    if (err == ESP_OK && (info.scene_count != 1 || info.dynamic_image_slots != 0)) err = ESP_ERR_NOT_SUPPORTED;
    esp_gsp_config_t config;
    if (err == ESP_OK) err = esp_gsp_deployable_bundle_make_config(s_card.bundle, &config);
    if (err == ESP_OK) {
        atomic_store(&s_card.active, true);
        err = esp_gsp_esp_lcd_suspend(hub, &s_card.session);
    }
    if (err == ESP_OK) {
        err = esp_gsp_esp_lcd_start_on_session_prepared(s_card.session, &config, prepare, &s_card, &s_card.app);
        /* Startup failure resumes the hub and consumes the session. */
        if (err != ESP_OK) { s_card.session = NULL; s_card.app = NULL; }
        else err = s_card.prepare_error;
    }
    if (err != ESP_OK) {
        ideas_presentation_close();
        if (s_card.bundle) esp_gsp_deployable_bundle_close(s_card.bundle);
        s_card.bundle = NULL;
    }
    return err;
}
esp_err_t ideas_presentation_show(esp_gsp_handle_t hub, const char *path)
{
    struct stat st;
    if (!hub || stat(path, &st) || st.st_size <= 0 || st.st_size > IDEAS_MAX_ASSET_BYTES)
        return ESP_ERR_INVALID_SIZE;
    void *data = heap_caps_aligned_alloc(64, st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return ESP_ERR_NO_MEM;
    FILE *f = fopen(path, "rb");
    bool read = f && fread(data, 1, (size_t)st.st_size, f) == (size_t)st.st_size && !ferror(f);
    if (f) fclose(f);
    esp_err_t err = read ? ideas_presentation_open(hub, data, st.st_size, NULL) : ESP_FAIL;
    if (err == ESP_OK) {
        ideas_presentation_enable();
        while (!atomic_load(&s_card.closed)) vTaskDelay(pdMS_TO_TICKS(50));
        ideas_presentation_close();
    }
    heap_caps_free(data); return err;
}
