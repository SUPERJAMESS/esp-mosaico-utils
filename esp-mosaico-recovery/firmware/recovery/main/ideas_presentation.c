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

/* Persistent storage keeps USB input safe during the renderer handoff. */
typedef struct { atomic_bool active, closed, pressed; } card_t;
static card_t s_card;
bool ideas_presentation_input(int32_t x, int32_t y, bool pressed)
{
    if (!atomic_load(&s_card.active)) return false;
    bool was_pressed = atomic_exchange(&s_card.pressed, pressed);
    if (!pressed && was_pressed && x >= 0 && x < 72 && y >= 0 && y < 72)
        atomic_store(&s_card.closed, true);
    return true;
}
static bool input(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui; (void)ctx;
    (void)ideas_presentation_input(x, y, pressed);
    /* A presentation is static information. No downloaded action can invoke
     * device operations; the reserved top-left area always returns to Vibe. */
    return true;
}
static void prepare(esp_gsp_handle_t ui, void *ctx)
{
    if (esp_gsp_set_input_interceptor(ui, input, ctx) != ESP_GSP_OK)
        atomic_store(&((card_t *)ctx)->closed, true);
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
    esp_gsp_deployable_bundle_t *bundle = NULL;
    esp_err_t err = read ? esp_gsp_deployable_bundle_open(data, st.st_size, true, &bundle) : ESP_FAIL;
    esp_gsp_deployable_info_t info = {.struct_size = sizeof(info)};
    if (err == ESP_OK) err = esp_gsp_deployable_bundle_get_info(bundle, &info);
    if (err == ESP_OK && (info.scene_count != 1 || info.dynamic_image_slots != 0)) err = ESP_ERR_NOT_SUPPORTED;
    esp_gsp_config_t config;
    if (err == ESP_OK) err = esp_gsp_deployable_bundle_make_config(bundle, &config);
    esp_gsp_esp_lcd_session_t *session = NULL;
    if (err == ESP_OK) {
        atomic_store(&s_card.closed, false);
        atomic_store(&s_card.pressed, false);
        atomic_store(&s_card.active, true);
        err = esp_gsp_esp_lcd_suspend(hub, &session);
    }
    if (err == ESP_OK) {
        esp_gsp_handle_t app;
        err = esp_gsp_esp_lcd_start_on_session_prepared(session, &config, prepare, &s_card, &app);
        if (err == ESP_OK) {
            while (!atomic_load(&s_card.closed)) vTaskDelay(pdMS_TO_TICKS(50));
            /* Retry a transient display fence timeout without freeing bytes
             * still borrowed by the active renderer. */
            do { err = esp_gsp_esp_lcd_resume(session, app, &hub); if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100)); }
            while (err != ESP_OK);
        }
    }
    atomic_store(&s_card.active, false);
    if (bundle) esp_gsp_deployable_bundle_close(bundle);
    heap_caps_free(data); return err;
}
