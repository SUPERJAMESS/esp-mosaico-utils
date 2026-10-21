// SPDX-License-Identifier: Apache-2.0
/* Reuse the SDK/NVS boundary while linking the production native controller. */
#define main settings_service_fixture_main
#include "main.c"
#undef main
#include "vibe_ui.h"
#include "vibe_ui_state.h"

static vui_handle_t ui;
static vibe_ui_t *controller;
static uint16_t frame[480 * 480];
static unsigned key_pending, saves;
static const char *feedback_directory;
static void ui_snapshot(void *ctx, vibe_snapshot_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    assert(vibe_settings_snapshot(&out->settings) == ESP_OK);
    out->storage_total = 512ULL * 1024 * 1024;
    out->nand_generation = 1;
}
static int ui_command(void *ctx, vibe_command_t command, const char *a, const char *b)
{
    (void)ctx; (void)command; (void)a; (void)b;
    return ESP_OK;
}
static int ui_setting(void *ctx, vibe_settings_command_t command, const char *key, const char *value)
{
    (void)ctx;
    int err = vibe_settings_request(command, key, value);
    if (command == VIBE_SETTINGS_SHARED_SET && err == ESP_OK) ++saves;
    return err;
}
static unsigned ui_key(void *ctx)
{
    (void)ctx;
    unsigned result = key_pending; key_pending = 0; return result;
}
static vibe_page_t page(void)
{
    vibe_ui_status_t value; vibe_ui_get_status(controller, &value); return value.page;
}
static void advance(unsigned ms, bool paint)
{
    for (unsigned i = 0; i < ms; i += 50) {
        vui_tick(ui, 50);
        if (paint) vui_render(ui, frame);
    }
    vui_render(ui, frame);
}
static void tap(int x, int y)
{
    vui_pointer(ui, x, y, true); vui_render(ui, frame);
    vui_pointer(ui, x, y, false); vui_render(ui, frame);
    advance(250, true);
}
static void press_key(unsigned key, unsigned ms)
{
    key_pending = key; advance(ms, true);
}
static void capture(const char *directory, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (unsigned i = 0; i < 480 * 480; ++i) {
        unsigned p = frame[i];
        unsigned char rgb[3] = {((p >> 11) & 31) * 255 / 31,
                                ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    fclose(file);
}
static void reset_feedback(void)
{
    assert(page() == VIBE_RESET && !restarts);
    advance(300, true);
    assert(controller->snapshot.settings.reset_phase == VIBE_RESET_RESTARTING);
    capture(feedback_directory, "reset-success");
    /* A key or second hold cannot interrupt the committed reset. */
    press_key(1, 300);
    assert(page() == VIBE_RESET);
}
static void open_editor(void)
{
    tap(390, 360); assert(page() == VIBE_SETTINGS);
    tap(240, 376); assert(page() == VIBE_SHARED);
    tap(240, 122); assert(page() == VIBE_SHARED_DETAIL);
    tap(240, 220); assert(page() == VIBE_SHARED_EDITOR);
}
static void finish_save(void)
{
    assert(task);
    void (*next)(void *) = task; task = NULL; next(task_arg);
    advance(300, true);
    assert(page() == VIBE_SHARED && saves == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *scenario = argv[1];
    bool ssid = !strcmp(scenario, "ssid");
    bool hex = !strcmp(scenario, "password-hex");
    bool short_password = !strcmp(scenario, "password-short");
    const char *key = ssid ? "wifi_ssid" : (hex || short_password) ? "wifi_password" : "city";
    char initial[65] = "Paris";
    if (ssid) { memset(initial, 's', 32); initial[32] = 0; }
    if (hex) { memset(initial, 'a', 64); initial[64] = 0; }
    if (short_password) initial[0] = 0;
    factory_locale_init(); assert(vibe_settings_init() == ESP_OK);
    execute(VIBE_SETTINGS_TIMEOUT, NULL, "60");
    execute(VIBE_SETTINGS_SHARED_SET, key, initial);
    const vibe_services_t services = {.snapshot = ui_snapshot, .command = ui_command,
                                      .setting = ui_setting, .key = ui_key};
    assert(vui_create(&ui) == VUI_OK);
    assert(vibe_ui_create(ui, &services, &controller) == VUI_OK);
    advance(300, true);
    if (!strncmp(scenario, "reset-", 6)) {
        bool zh = !strcmp(scenario, "reset-zh");
        if (zh) { execute(VIBE_SETTINGS_LANGUAGE, NULL, "2"); advance(300, true); }
        vibe_show(controller, VIBE_RESET); advance(300, true);
        vui_pointer(ui, 240, 396, true); advance(3100, true);
        assert(task && !restarts && s_settings.reset_phase == VIBE_RESET_CLEARING);
        advance(300, true);
        assert(controller->snapshot.settings.reset_phase == VIBE_RESET_CLEARING);
        capture(argv[2], "reset-clearing");
        vui_pointer(ui, 240, 396, false);
        feedback_directory = argv[2]; reset_feedback_hook = reset_feedback;
        void (*next)(void *) = task; task = NULL; next(task_arg);
        assert(restarts == 1 && reset_feedbacks == 1 && !s_settings.error);
        assert(!s_settings.shared_count && s_settings.language == 1);
        vibe_ui_deinit(controller); vui_destroy(ui); free(s_lock); s_lock = NULL;
        return 0;
    }
    open_editor();
    tap(35, 250);
    capture(argv[2], "before");
    if (!strcmp(scenario, "boot") || !strcmp(scenario, "boot-sleep") || !strcmp(scenario, "boot-cancel")) {
        press_key(3, 100); assert(page() == VIBE_BOOT_TIP);
        if (!strcmp(scenario, "boot-sleep")) {
            advance(62000, false); assert(page() == VIBE_SLEEP);
            press_key(4, 100); assert(page() == VIBE_SLEEP);
            press_key(1, 600);
        } else if (!strcmp(scenario, "boot-cancel")) {
            press_key(1, 300); assert(page() == VIBE_HOME); open_editor();
        } else press_key(4, 300);
        assert(page() == VIBE_SHARED_EDITOR);
    } else if (!strcmp(scenario, "sleep")) {
        advance(62000, false); assert(page() == VIBE_SLEEP);
        press_key(1, 600); assert(page() == VIBE_SHARED_EDITOR);
    } else if (short_password) {
        tap(430, 430);
        assert(page() == VIBE_SHARED_EDITOR && !task && !saves);
        for (unsigned i = 0; i < 7; ++i) tap(35, 250);
    } else assert(ssid || hex);
    capture(argv[2], "returned");
    tap(430, 430); finish_save();
    char saved[257];
    assert(mosaico_shared_get(key, saved, sizeof(saved)) == ESP_OK);
    if (ssid) assert(strlen(saved) == 32 && !strcmp(saved, initial));
    else if (hex) assert(strlen(saved) == 64 && !strcmp(saved, initial));
    else if (short_password) assert(!strcmp(saved, "qqqqqqqq"));
    else assert(!strcmp(saved, !strcmp(scenario, "boot-cancel") ? "Paris" : "Parisq"));
    capture(argv[2], "saved");
    printf("scenario=%s saved_length=%zu page=%u\n", scenario, strlen(saved), page());
    vibe_ui_deinit(controller); vui_destroy(ui);
    free(s_lock); s_lock = NULL;
    return 0;
}
