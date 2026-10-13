// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "app_hello.h"
#include "bundle_gsp.h"

struct fake_ui {
    esp_gsp_event_cb_t event;
    esp_gsp_timer_cb_t timer;
    void *event_ctx;
    void *timer_ctx;
    bool fail_setter, fail_register, fail_delete, fail_detach, fail_allocate_timer;
    bool celebrating;
    char text[4];
};

bool gsp_hello_event_is_say_hello(const esp_gsp_event_t *event) { return event && event->hello; }
esp_gsp_err_t gsp_hello_count_set_text(esp_gsp_handle_t ui, const char *text)
{
    if (ui->fail_setter) return ESP_GSP_ERR_INVALID_STATE;
    strcpy(ui->text, text);
    return ESP_GSP_OK;
}
esp_gsp_err_t gsp_hello_celebration_set_visible(esp_gsp_handle_t ui, bool visible)
{
    ui->celebrating = visible;
    return ESP_GSP_OK;
}
#define CONFETTI(N) \
    esp_gsp_err_t gsp_hello_confetti_##N##_set_visible(esp_gsp_handle_t ui, bool visible) \
    { (void)ui; (void)visible; return ESP_GSP_OK; }
CONFETTI(0) CONFETTI(1) CONFETTI(2) CONFETTI(3) CONFETTI(4) CONFETTI(5)
esp_gsp_err_t esp_gsp_on_event(esp_gsp_handle_t ui, esp_gsp_event_cb_t callback, void *ctx)
{
    if ((callback && ui->fail_register) || (!callback && ui->fail_detach)) return ESP_GSP_ERR_INVALID_STATE;
    ui->event = callback;
    ui->event_ctx = ctx;
    return ESP_GSP_OK;
}
void *esp_gsp_timer_create(esp_gsp_handle_t ui, uint32_t ms, esp_gsp_timer_cb_t callback, void *ctx)
{
    assert(ms == 100);
    if (ui->fail_allocate_timer) return NULL;
    ui->timer = callback;
    ui->timer_ctx = ctx;
    return ui;
}
esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t ui, void *timer)
{
    assert(timer == ui);
    if (ui->fail_delete) return ESP_GSP_ERR_INVALID_STATE;
    ui->timer = NULL;
    ui->timer_ctx = NULL;
    return ESP_GSP_OK;
}
static void click(esp_gsp_handle_t ui)
{
    const esp_gsp_event_t event = {true};
    assert(ui->event);
    ui->event(ui, &event, ui->event_ctx);
}
int main(void)
{
    struct fake_ui ui = {0};
    app_hello_handle_t controller = NULL;
    app_hello_snapshot_t snapshot;
    assert(app_hello_create(NULL, &controller) == ESP_GSP_ERR_INVALID_ARG);
    assert(app_hello_create(&ui, NULL) == ESP_GSP_ERR_INVALID_ARG);
    ui.fail_setter = true;
    assert(app_hello_create(&ui, &controller) == ESP_GSP_ERR_INVALID_STATE && !controller && !ui.event);
    ui.fail_setter = false;
    ui.fail_register = true;
    assert(app_hello_create(&ui, &controller) == ESP_GSP_ERR_INVALID_STATE && !controller && !ui.event);
    ui.fail_register = false;
    assert(app_hello_create(&ui, &controller) == ESP_GSP_OK);
    assert(app_hello_read(controller, NULL) == ESP_GSP_ERR_INVALID_ARG);
    assert(app_hello_read(NULL, &snapshot) == ESP_GSP_ERR_INVALID_ARG);
    click(&ui);
    assert(!strcmp(ui.text, "01"));
    assert(app_hello_read(controller, &snapshot) == ESP_GSP_OK && snapshot.count == 1);
    snapshot.count = 99; /* Public snapshots cannot mutate the controller. */
    assert(app_hello_read(controller, &snapshot) == ESP_GSP_OK && snapshot.count == 1);
    for (int i = 1; i < 99; ++i) click(&ui);
    ui.fail_allocate_timer = true;
    click(&ui);
    assert(app_hello_read(controller, &snapshot) == ESP_GSP_OK);
    assert(snapshot.count == 99 && !snapshot.celebrating && snapshot.last_error == ESP_GSP_ERR_NO_MEM);
    ui.fail_allocate_timer = false;
    click(&ui);
    assert(ui.celebrating && ui.timer);
    click(&ui); /* Input during celebration does not increment the count. */
    assert(app_hello_read(controller, &snapshot) == ESP_GSP_OK && snapshot.count == 100);
    for (int i = 0; i < 29; ++i) ui.timer(&ui, ui.timer_ctx);
    ui.fail_delete = true;
    ui.timer(&ui, ui.timer_ctx);
    assert(app_hello_read(controller, &snapshot) == ESP_GSP_OK);
    assert(snapshot.count == 100 && snapshot.celebrating && ui.celebrating);
    ui.fail_delete = false;
    ui.timer(&ui, ui.timer_ctx);
    assert(!ui.timer && !ui.celebrating && !strcmp(ui.text, "00"));
    for (int i = 0; i < 100; ++i) click(&ui);
    ui.fail_detach = true;
    assert(app_hello_destroy(&controller) == ESP_GSP_ERR_INVALID_STATE && controller);
    ui.fail_detach = false;
    ui.fail_delete = true;
    assert(app_hello_destroy(&controller) == ESP_GSP_ERR_INVALID_STATE && controller && ui.timer);
    ui.fail_delete = false;
    assert(app_hello_destroy(&controller) == ESP_GSP_OK && !controller && !ui.timer && !ui.event);
    assert(app_hello_destroy(&controller) == ESP_GSP_OK);
    assert(app_hello_destroy(NULL) == ESP_GSP_ERR_INVALID_ARG);
    return 0;
}
