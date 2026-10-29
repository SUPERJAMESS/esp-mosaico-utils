// SPDX-License-Identifier: Apache-2.0

#include "app_hello.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"

#define CELEBRATION_TICK_MS 100
#define CELEBRATION_TICKS 30
#define CONFETTI_FRAMES 6
#define UI_PERIOD_MS 32
#define COLOR_DIM 0x1A1A1Cu
#define COLOR_LIT 0xFF4C01u

struct app_hello {
    esp_gsp_handle_t ui;
    uint32_t count;
    void *timer;
    void *ui_timer;
    unsigned celebration_tick;
    unsigned confetti_frame;
    app_hello_pref_cb_t on_pref;
    app_hello_activity_cb_t on_activity;
    void *handler_ctx;
    app_vibe_settings_view_t settings_buf[2];
    int settings_published;
    volatile int toggle_req;
    volatile int view_dirty;
    app_vibe_settings_view_t settings;
    int page;
    bool settings_open;
    int drag_well;
    int32_t drag_y;
    int32_t drag_origin_y;
    bool drag_moved;
    bool level_dirty;
    char pending_pass[65];
    esp_gsp_list_t networks;
    esp_gsp_err_t last_error;
};

static esp_gsp_err_t (*const confetti_visible[])(esp_gsp_handle_t, bool) = {
    gsp_hello_confetti_0_set_visible, gsp_hello_confetti_1_set_visible,
    gsp_hello_confetti_2_set_visible, gsp_hello_confetti_3_set_visible,
    gsp_hello_confetti_4_set_visible, gsp_hello_confetti_5_set_visible,
};

static void remember_error(struct app_hello *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

static void post_pref(struct app_hello *state, app_vibe_cmd_t cmd, uint32_t value)
{
    if (state->on_pref) {
        state->on_pref(cmd, value, state->handler_ctx);
    }
}

static void note_activity(struct app_hello *state)
{
    if (state->on_activity) {
        state->on_activity(state->handler_ctx);
    }
}

static void mark_choice(struct app_hello *state, gsp_component_key_t key, bool on)
{
    remember_error(state, esp_gsp_component_set_color_rgb888(
                              state->ui, key, on ? COLOR_LIT : COLOR_DIM));
}

static void text(struct app_hello *state, gsp_component_key_t key, const char *value)
{
    remember_error(state, esp_gsp_component_set_text(state->ui, key, value ? value : ""));
}

static uint8_t snap_level(int32_t value, const uint8_t *levels, size_t count)
{
    uint8_t best = levels[0];
    uint32_t best_dist = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        int32_t delta = value - (int32_t)levels[i];
        uint32_t dist = (uint32_t)(delta < 0 ? -delta : delta);
        if (dist < best_dist) {
            best_dist = dist;
            best = levels[i];
        }
    }
    return best;
}

static uint8_t step_level(uint8_t value, const uint8_t *levels, size_t count, bool increase)
{
    size_t nearest = 0;
    for (size_t i = 1; i < count; ++i) {
        if (value >= levels[i]) {
            nearest = i;
        }
    }
    if (increase) {
        return levels[nearest + (nearest + 1 < count)];
    }
    return levels[nearest - (nearest > 0)];
}

static const char *tr(const struct app_hello *state, const char *en, const char *zh)
{
    return state->settings.lang == 2 ? zh : en;
}

static const char *wifi_phrase(const struct app_hello *state, const char *value)
{
    static const char *en[] = {
        "Not connected", "Choose a network below", "Scanning", "Scan failed",
        "Connecting", "Check the password", "Saved", "Open", "Secured",
    };
    static const char *zh[] = {
        "未连接", "请选择下方网络", "正在扫描", "扫描失败",
        "正在连接", "请检查密码后再试", "已保存", "开放", "加密",
    };
    if (!value) {
        return "";
    }
    if (state->settings.lang != 2) {
        return value;
    }
    for (size_t i = 0; i < sizeof(en) / sizeof(en[0]); ++i) {
        if (strcmp(value, en[i]) == 0) {
            return zh[i];
        }
    }
    return value;
}

static void show_page(struct app_hello *state)
{
    static const gsp_component_key_t pages[] = {
        GSP_HELLO_OBJ_KEY_MENU,          GSP_HELLO_OBJ_KEY_PAGE_DISPLAY,
        GSP_HELLO_OBJ_KEY_PAGE_LANG,     GSP_HELLO_OBJ_KEY_PAGE_HAPTICS,
        GSP_HELLO_OBJ_KEY_PAGE_WIFI,     GSP_HELLO_OBJ_KEY_PAGE_PASSWORD,
        GSP_HELLO_OBJ_KEY_PAGE_VOLUME,
    };
    if (state->page < 0 || state->page > 6) {
        state->page = 0;
    }
    for (int i = 0; i < 7; ++i) {
        remember_error(state, esp_gsp_component_set_visible(
                                  state->ui, pages[i], i == state->page));
    }
}

static uint8_t s_shown_volume = 255;
static uint8_t s_shown_brightness = 255;

static void paint_settings(struct app_hello *state)
{
    char volume[4];
    char brightness[4];
    static const uint8_t k_level[] = {20, 40, 60, 80, 100};
    static const uint16_t k_off[] = {0, 60, 180, 300, 600};
    static const uint8_t k_vol[] = {0, 20, 40, 60, 80, 100};
    static const gsp_component_key_t k_sleep[] = {
        GSP_HELLO_OBJ_KEY_TO0, GSP_HELLO_OBJ_KEY_TO60, GSP_HELLO_OBJ_KEY_TO180,
        GSP_HELLO_OBJ_KEY_TO300, GSP_HELLO_OBJ_KEY_TO600,
    };
    static const gsp_component_key_t k_volume[] = {
        GSP_HELLO_OBJ_KEY_VOL0, GSP_HELLO_OBJ_KEY_VOL20, GSP_HELLO_OBJ_KEY_VOL40,
        GSP_HELLO_OBJ_KEY_VOL60, GSP_HELLO_OBJ_KEY_VOL80, GSP_HELLO_OBJ_KEY_VOL100,
    };
    static const gsp_component_key_t k_volume_fill[] = {
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_0, GSP_HELLO_OBJ_KEY_VOLUME_FILL_20,
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_40, GSP_HELLO_OBJ_KEY_VOLUME_FILL_60,
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_80, GSP_HELLO_OBJ_KEY_VOLUME_FILL_100,
    };
    static const gsp_component_key_t k_brightness_fill[] = {
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_20, GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_40,
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_60, GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_80,
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_100,
    };
    show_page(state);
    text(state, GSP_HELLO_OBJ_KEY_OPEN_SETTINGS, tr(state, "Settings", "设置"));
    text(state, GSP_HELLO_OBJ_KEY_SET_TITLE, tr(state, "Settings", "设置"));
    text(state, GSP_HELLO_OBJ_KEY_NAV_WIFI, tr(state, "Wi-Fi", "配网"));
    text(state, GSP_HELLO_OBJ_KEY_NAV_DISPLAY, tr(state, "Brightness", "亮度"));
    text(state, GSP_HELLO_OBJ_KEY_NAV_LANG, tr(state, "Language", "语言"));
    text(state, GSP_HELLO_OBJ_KEY_NAV_HAPTICS, tr(state, "Haptics", "马达"));
    text(state, GSP_HELLO_OBJ_KEY_NAV_VOLUME, tr(state, "Volume", "音量"));
    text(state, GSP_HELLO_OBJ_KEY_MENU_WIFI_LINE, wifi_phrase(state, state->settings.wifi_line));
    snprintf(volume, sizeof(volume), "%u", state->settings.volume);
    snprintf(brightness, sizeof(brightness), "%u", state->settings.brightness);
    text(state, GSP_HELLO_OBJ_KEY_MENU_VOLUME_VALUE, volume);
    text(state, GSP_HELLO_OBJ_KEY_MENU_BRIGHTNESS_VALUE, brightness);
    text(state, GSP_HELLO_OBJ_KEY_MENU_LANG_SUB,
         state->settings.lang == 2 ? "简体中文" : "English");
    text(state, GSP_HELLO_OBJ_KEY_MENU_ROTATE_TITLE, tr(state, "Rotate", "旋转"));
    text(state, GSP_HELLO_OBJ_KEY_MENU_SLEEP_TITLE, tr(state, "Screen off", "息屏"));
    text(state, GSP_HELLO_OBJ_KEY_MENU_HAPTIC_SUB,
         state->settings.haptics ? tr(state, "On", "开") : tr(state, "Off", "关"));
    text(state, GSP_HELLO_OBJ_KEY_MENU_ROTATE_SUB,
         state->settings.rotate ? tr(state, "On", "开") : tr(state, "Off", "关"));
    text(state, GSP_HELLO_OBJ_KEY_MENU_SLEEP_SUB,
         state->settings.timeout_s == 0 ? tr(state, "Never", "永不") :
         state->settings.timeout_s == 60 ? tr(state, "1 minute", "1 分钟") :
         state->settings.timeout_s == 180 ? tr(state, "3 minutes", "3 分钟") :
         state->settings.timeout_s == 300 ? tr(state, "5 minutes", "5 分钟") :
                                             tr(state, "10 minutes", "10 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_DISP_TITLE, tr(state, "Screen off", "息屏"));
    text(state, GSP_HELLO_OBJ_KEY_SET_SLEEP, tr(state, "Choose when the screen sleeps", "息屏 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_TO0_TEXT, tr(state, "Never", "永不"));
    text(state, GSP_HELLO_OBJ_KEY_TO60_TEXT, tr(state, "1 minute", "1 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_TO180_TEXT, tr(state, "3 minutes", "3 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_TO300_TEXT, tr(state, "5 minutes", "5 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_TO600_TEXT, tr(state, "10 minutes", "10 分钟"));
    text(state, GSP_HELLO_OBJ_KEY_ROTATE_TOGGLE,
         state->settings.rotate ? tr(state, "Auto rotate: on", "自动旋转:开")
                                : tr(state, "Auto rotate: off", "自动旋转:关"));
    text(state, GSP_HELLO_OBJ_KEY_LANG_TITLE, tr(state, "Language", "语言"));
    text(state, GSP_HELLO_OBJ_KEY_HAP_TITLE, tr(state, "Haptics", "马达"));
    text(state, GSP_HELLO_OBJ_KEY_VOL_TITLE, tr(state, "Volume", "音量"));
    text(state, GSP_HELLO_OBJ_KEY_WIFI_SCAN, tr(state, "Scan", "扫描"));
    text(state, GSP_HELLO_OBJ_KEY_WIFI_FORGET, tr(state, "Forget", "忘记"));
    text(state, GSP_HELLO_OBJ_KEY_WIFI_LINE, wifi_phrase(state, state->settings.wifi_line));
    text(state, GSP_HELLO_OBJ_KEY_WIFI_DETAIL, wifi_phrase(state, state->settings.wifi_detail));
    text(state, GSP_HELLO_OBJ_KEY_PWD_SSID, state->settings.wifi_ssid);
    for (int i = 0; i < 5; ++i) {
        mark_choice(state, k_sleep[i], state->settings.timeout_s == k_off[i]);
        remember_error(state, esp_gsp_component_set_visible(
                                  state->ui, k_brightness_fill[i],
                                  state->settings.brightness == k_level[i]));
    }
    for (int i = 0; i < 6; ++i) {
        remember_error(state, esp_gsp_component_set_visible(
                                  state->ui, k_volume_fill[i],
                                  state->settings.volume == k_vol[i]));
    }
    mark_choice(state, GSP_HELLO_OBJ_KEY_MENU_HAPTIC_TILE, state->settings.haptics);
    mark_choice(state, GSP_HELLO_OBJ_KEY_MENU_ROTATE_TILE, state->settings.rotate);
    for (int i = 0; i < 6; ++i) {
        mark_choice(state, k_volume[i], state->settings.volume == k_vol[i]);
    }
    mark_choice(state, GSP_HELLO_OBJ_KEY_LANG_EN, state->settings.lang != 2);
    mark_choice(state, GSP_HELLO_OBJ_KEY_LANG_ZH, state->settings.lang == 2);
    mark_choice(state, GSP_HELLO_OBJ_KEY_HAP_ON, state->settings.haptics);
    mark_choice(state, GSP_HELLO_OBJ_KEY_HAP_OFF, !state->settings.haptics);
    s_shown_volume = state->settings.volume;
    s_shown_brightness = state->settings.brightness;
    if (state->networks != ESP_GSP_LIST_NONE) {
        remember_error(state, esp_gsp_list_set_total(
                                  state->ui, state->networks, state->settings.wifi_ap_count));
        remember_error(state, esp_gsp_list_refresh(state->ui, state->networks));
    }
}

static gsp_err_t bind_network(esp_gsp_handle_t gsp, esp_gsp_row_t row,
                             uint32_t index, void *ctx)
{
    struct app_hello *state = ctx;
    const char *title = "";
    const char *detail = wifi_phrase(state, "Secured");
    if (index < state->settings.wifi_ap_count && index < 12) {
        title = state->settings.wifi_ap[index];
        detail = wifi_phrase(state, state->settings.wifi_open[index] ? "Open" : "Secured");
    }
    remember_error(state, esp_gsp_row_set_text(gsp, row, 0, title));
    return esp_gsp_row_set_text(gsp, row, 1, detail);
}

static void celebrate_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    if (++state->celebration_tick >= CELEBRATION_TICKS) {
        const esp_gsp_err_t err = esp_gsp_timer_delete(ui, state->timer);
        remember_error(state, err);
        if (err != ESP_GSP_OK) {
            return;
        }
        state->timer = NULL;
        remember_error(state, gsp_hello_count_set_text(ui, "00"));
        remember_error(state, gsp_hello_celebration_set_visible(ui, false));
        state->count = 0;
        return;
    }
    remember_error(state, confetti_visible[state->confetti_frame](ui, false));
    state->confetti_frame = (state->confetti_frame + 1) % CONFETTI_FRAMES;
    remember_error(state, confetti_visible[state->confetti_frame](ui, true));
}

static void say_hello(struct app_hello *state)
{
    if (state->timer) {
        return;
    }
    note_activity(state);
    if (state->count == 99) {
        state->celebration_tick = 0;
        state->timer = esp_gsp_timer_create(state->ui, CELEBRATION_TICK_MS,
                                            celebrate_tick, state);
        if (!state->timer) {
            remember_error(state, ESP_GSP_ERR_NO_MEM);
            return;
        }
        remember_error(state, gsp_hello_celebration_set_visible(state->ui, true));
        state->count = 100;
        return;
    }
    char text[3];
    const uint32_t next = state->count + 1;
    snprintf(text, sizeof(text), "%02u", (unsigned)next);
    const esp_gsp_err_t err = gsp_hello_count_set_text(state->ui, text);
    remember_error(state, err);
    if (err == ESP_GSP_OK) {
        state->count = next;
    }
}

static void bounce_ai_mark(struct app_hello *state, bool play)
{
    esp_gsp_err_t err;
    if (!play) {
        err = esp_gsp_component_stop_animation(state->ui, GSP_HELLO_OBJ_KEY_AI_MARK,
                                               GSP_PROPERTY_KEY_Y);
        if (err != ESP_GSP_OK && err != ESP_GSP_ERR_NOT_SUPPORTED) {
            remember_error(state, err);
        }
        return;
    }
    const gsp_value_t from = {.type = GSP_VALUE_I32, .data.i32 = 36};
    const gsp_value_t to = {.type = GSP_VALUE_I32, .data.i32 = 32};
    const esp_gsp_animation_config_t config = {
        .duration_ms = 420,
        .iterations = 0,
        .alternate = true,
        .ease = ESP_GSP_EASE_IN_OUT,
    };
    err = esp_gsp_component_play_animation(state->ui, GSP_HELLO_OBJ_KEY_AI_MARK,
                                           GSP_PROPERTY_KEY_Y, &from, &to, &config);
    if (err != ESP_GSP_OK && err != ESP_GSP_ERR_NOT_SUPPORTED) {
        remember_error(state, err);
    }
}

static void set_open(struct app_hello *state, bool open)
{
    state->settings_open = open;
    if (open) {
        state->page = 0;
        paint_settings(state);
        post_pref(state, APP_VIBE_CMD_RELOAD, 0);
    }
    remember_error(state, esp_gsp_component_set_visible(
                              state->ui, GSP_HELLO_OBJ_KEY_SETTINGS, open));
    bounce_ai_mark(state, open);
}

static void show_one_fill(struct app_hello *state, const gsp_component_key_t *keys,
                          const uint8_t *levels, size_t count, uint8_t *shown, uint8_t value)
{
    if (*shown == value) {
        return;
    }
    /* Show the new level before hiding the old one so the card never flashes empty. */
    for (size_t i = 0; i < count; ++i) {
        if (levels[i] == value) {
            remember_error(state, esp_gsp_component_set_visible(state->ui, keys[i], true));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (levels[i] == *shown && levels[i] != value) {
            remember_error(state, esp_gsp_component_set_visible(state->ui, keys[i], false));
        }
    }
    *shown = value;
}

static void paint_level(struct app_hello *state)
{
    char digits[4];
    static const uint8_t k_vol[] = {0, 20, 40, 60, 80, 100};
    static const uint8_t k_level[] = {20, 40, 60, 80, 100};
    static const gsp_component_key_t k_volume_fill[] = {
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_0, GSP_HELLO_OBJ_KEY_VOLUME_FILL_20,
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_40, GSP_HELLO_OBJ_KEY_VOLUME_FILL_60,
        GSP_HELLO_OBJ_KEY_VOLUME_FILL_80, GSP_HELLO_OBJ_KEY_VOLUME_FILL_100,
    };
    static const gsp_component_key_t k_brightness_fill[] = {
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_20, GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_40,
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_60, GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_80,
        GSP_HELLO_OBJ_KEY_BRIGHTNESS_FILL_100,
    };
    if (s_shown_volume != state->settings.volume) {
        snprintf(digits, sizeof(digits), "%u", state->settings.volume);
        text(state, GSP_HELLO_OBJ_KEY_MENU_VOLUME_VALUE, digits);
        show_one_fill(state, k_volume_fill, k_vol, 6, &s_shown_volume, state->settings.volume);
    }
    if (s_shown_brightness != state->settings.brightness) {
        snprintf(digits, sizeof(digits), "%u", state->settings.brightness);
        text(state, GSP_HELLO_OBJ_KEY_MENU_BRIGHTNESS_VALUE, digits);
        show_one_fill(state, k_brightness_fill, k_level, 5, &s_shown_brightness,
                      state->settings.brightness);
    }
}

static void apply_drag_level(struct app_hello *state, bool volume, int32_t raw)
{
    static const uint8_t k_vol[] = {0, 20, 40, 60, 80, 100};
    static const uint8_t k_brightness[] = {20, 40, 60, 80, 100};
    const uint8_t *levels = volume ? k_vol : k_brightness;
    uint8_t level = snap_level(raw, levels, volume ? 6 : 5);
    uint8_t *slot = volume ? &state->settings.volume : &state->settings.brightness;
    if (*slot == level) {
        return;
    }
    *slot = level;
    state->level_dirty = true;
    post_pref(state, APP_VIBE_CMD_HAPTIC_FEEDBACK, 0);
    paint_level(state);
}

/* Matches the home wells in ui/main.json. Tiles start at y=340, below this box. */
#define WELL_Y 158
#define WELL_H 158
#define VOL_X0 20
#define VOL_X1 236
#define BRI_X0 244
#define BRI_X1 460

static int classify_well(int32_t x, int32_t y)
{
    if (y < WELL_Y || y >= WELL_Y + WELL_H) {
        return -1;
    }
    if (x >= VOL_X0 && x < VOL_X1) {
        return 1;
    }
    if (x >= BRI_X0 && x < BRI_X1) {
        return 2;
    }
    return -1;
}

static void on_pointer(esp_gsp_handle_t gsp, int32_t x, int32_t y, bool pressed, void *user_ctx)
{
    struct app_hello *state = user_ctx;
    int32_t from_top;
    (void)gsp;
    if (!pressed) {
        if (state->drag_moved && state->level_dirty) {
            if (state->drag_well == 1) {
                post_pref(state, APP_VIBE_CMD_VOLUME, state->settings.volume);
            } else if (state->drag_well == 2) {
                post_pref(state, APP_VIBE_CMD_BRIGHTNESS, state->settings.brightness);
            }
        }
        state->drag_well = 0;
        state->drag_moved = false;
        state->level_dirty = false;
        return;
    }
    if (!state->settings_open || state->page != 0) {
        return;
    }
    /* A new contact starts outside, or jumps onto the tiles after a missed release.
     * Movement that stays inside a well is still the same drag. */
    if (state->drag_well == 0 ||
        (classify_well(x, y) < 0 && (y > state->drag_y + 64 || y < state->drag_y - 64))) {
        if (state->drag_moved && state->level_dirty) {
            if (state->drag_well == 1) {
                post_pref(state, APP_VIBE_CMD_VOLUME, state->settings.volume);
            } else if (state->drag_well == 2) {
                post_pref(state, APP_VIBE_CMD_BRIGHTNESS, state->settings.brightness);
            }
        }
        state->drag_well = classify_well(x, y);
        state->drag_y = y;
        state->drag_origin_y = y;
        state->drag_moved = false;
        state->level_dirty = false;
        if (state->drag_well > 0) {
            note_activity(state);
        }
        return;
    }
    state->drag_y = y;
    if (state->drag_well < 1) {
        return;
    }
    if (y > state->drag_origin_y + 8 || y < state->drag_origin_y - 8) {
        state->drag_moved = true;
    }
    if (!state->drag_moved) {
        return;
    }
    from_top = y - WELL_Y;
    if (from_top < 0) {
        from_top = 0;
    } else if (from_top > WELL_H - 1) {
        from_top = WELL_H - 1;
    }
    apply_drag_level(state, state->drag_well == 1,
                     (WELL_H - 1 - from_top) * 100 / (WELL_H - 1));
}

static void on_ui(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    (void)ui;
    if (state->toggle_req) {
        state->toggle_req = 0;
        set_open(state, !state->settings_open);
    }
    if (state->view_dirty) {
        app_vibe_settings_view_t *next = &state->settings_buf[state->settings_published & 1];
        if (state->drag_well > 0) {
            next->volume = state->settings.volume;
            next->brightness = state->settings.brightness;
        }
        uint8_t volume = state->settings.volume;
        uint8_t brightness = state->settings.brightness;
        state->view_dirty = 0;
        state->settings.volume = next->volume;
        state->settings.brightness = next->brightness;
        bool only_level = memcmp(&state->settings, next, sizeof(*next)) == 0;
        state->settings.volume = volume;
        state->settings.brightness = brightness;
        bool changed = memcmp(&state->settings, next, sizeof(*next)) != 0;
        if (changed) {
            state->settings = *next;
            if (state->settings_open) {
                if (only_level) {
                    paint_level(state);
                } else {
                    paint_settings(state);
                }
            }
        }
    }
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    struct app_hello *state = ctx;
    int32_t arg;
    if (!event) {
        return;
    }
    if (gsp_hello_event_is_say_hello(event)) {
        if (!state->settings_open) {
            say_hello(state);
        }
        return;
    }
    if (gsp_hello_event_is_open_settings(event)) {
        note_activity(state);
        set_open(state, true);
        return;
    }
    if (gsp_hello_event_is_password_keyboard_key(event)) {
        char secret[65];
        note_activity(state);
        if (esp_gsp_keyboard_text(ui, secret, sizeof(secret)) == ESP_GSP_OK) {
            memset(state->pending_pass, 0, sizeof(state->pending_pass));
            memcpy(state->pending_pass, secret, sizeof(state->pending_pass) - 1);
            memset(secret, 0, sizeof(secret));
            post_pref(state, APP_VIBE_CMD_WIFI_JOIN, 0);
        }
        return;
    }
    if (gsp_hello_event_is_select_network(event)) {
        note_activity(state);
        if (event->item < state->settings.wifi_ap_count) {
            snprintf(state->settings.wifi_ssid, sizeof(state->settings.wifi_ssid), "%s",
                     state->settings.wifi_ap[event->item]);
            post_pref(state, APP_VIBE_CMD_WIFI_PICK, event->item);
            state->page = 5;
            paint_settings(state);
        }
        return;
    }
    if (!gsp_hello_event_is_settings_action(event)) {
        return;
    }
    note_activity(state);
    arg = (int32_t)event->arg;
    if (arg == 4000) {
        set_open(state, false);
        return;
    }
    if (arg == 4001) {
        state->page = 0;
    } else if (arg == 4002) {
        state->page = 4;
    } else if (arg >= 5000 && arg <= 5004) {
        static const int k_page[] = {4, 1, 2, 3, 6};
        state->page = k_page[arg - 5000];
        if (arg == 5000) {
            post_pref(state, APP_VIBE_CMD_RELOAD, 0);
        }
    } else if (arg >= 8000 && arg <= 8100 && arg % 20 == 0) {
        post_pref(state, APP_VIBE_CMD_VOLUME, (uint32_t)(arg - 8000));
    } else if (arg == 6000 || arg == 6001) {
        static const uint8_t k_vol[] = {0, 20, 40, 60, 80, 100};
        post_pref(state, APP_VIBE_CMD_VOLUME,
                  step_level(state->settings.volume, k_vol, 6, arg == 6001));
    } else if (arg == 6100 || arg == 6101) {
        static const uint8_t k_brightness[] = {20, 40, 60, 80, 100};
        post_pref(state, APP_VIBE_CMD_BRIGHTNESS,
                  step_level(state->settings.brightness, k_brightness, 5, arg == 6101));
    } else if (arg == 20 || arg == 40 || arg == 60 || arg == 80 || arg == 100) {
        post_pref(state, APP_VIBE_CMD_BRIGHTNESS, (uint32_t)arg);
    } else if (arg == 10000 || arg == 10060 || arg == 10180 || arg == 10300 || arg == 10600) {
        post_pref(state, APP_VIBE_CMD_TIMEOUT, (uint32_t)(arg - 10000));
    } else if (arg == 2000) {
        post_pref(state, APP_VIBE_CMD_ROTATE, state->settings.rotate ? 0U : 1U);
    } else if (arg == 3001 || arg == 3002) {
        post_pref(state, APP_VIBE_CMD_LANG, (uint32_t)(arg - 3000));
    } else if (arg == 7000 || arg == 7001) {
        post_pref(state, APP_VIBE_CMD_HAPTICS, arg == 7001 ? 1U : 0U);
    } else if (arg == 7002) {
        post_pref(state, APP_VIBE_CMD_HAPTICS, state->settings.haptics ? 0U : 1U);
    } else if (arg == 5100) {
        post_pref(state, APP_VIBE_CMD_WIFI_SCAN, 0);
    } else if (arg == 5500) {
        post_pref(state, APP_VIBE_CMD_WIFI_FORGET, 0);
    } else {
        return;
    }
    paint_settings(state);
}

esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;
    state->settings.brightness = 80;
    state->settings.volume = 80;
    state->settings.haptics = true;
    state->settings.lang = 1;
    snprintf(state->settings.wifi_line, sizeof(state->settings.wifi_line), "Not connected");
    snprintf(state->settings.wifi_detail, sizeof(state->settings.wifi_detail),
             "Choose a network below");
    state->networks = ESP_GSP_LIST_NONE;
    remember_error(state, gsp_hello_count_set_text(ui, "00"));
    remember_error(state, gsp_hello_celebration_set_visible(ui, false));
    for (unsigned i = 0; i < CONFETTI_FRAMES; ++i) {
        remember_error(state, confetti_visible[i](ui, i == 0));
    }
    esp_gsp_err_t err = state->last_error;
    if (err == ESP_GSP_OK) {
        err = esp_gsp_on_event(ui, on_event, state);
    }
    if (err == ESP_GSP_OK) {
        esp_gsp_err_t pointer_err = esp_gsp_set_pointer_observer(ui, on_pointer, state);
        if (pointer_err != ESP_GSP_OK && pointer_err != ESP_GSP_ERR_NOT_SUPPORTED) {
            err = pointer_err;
        }
    }
    if (err != ESP_GSP_OK) {
        free(state);
        return err;
    }
    state->networks = esp_gsp_list_bind_component(
        ui, GSP_HELLO_OBJ_KEY_NETWORKS, bind_network, state);
    if (state->networks == ESP_GSP_LIST_NONE) {
        remember_error(state, ESP_GSP_ERR_INVALID_STATE);
    }
    (void)esp_gsp_keyboard_attach_ex(ui, GSP_HELLO_ACTION_PASSWORD_KEYBOARD_KEY,
                                     GSP_HELLO_BIND_PASSWORD_DISPLAY, 64);
    remember_error(state, esp_gsp_component_set_visible(ui, GSP_HELLO_OBJ_KEY_SETTINGS, false));
    state->ui_timer = esp_gsp_timer_create(ui, UI_PERIOD_MS, on_ui, state);
    if (!state->ui_timer) {
        (void)esp_gsp_on_event(ui, NULL, NULL);
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot)
{
    if (!handle || !out_snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_hello_snapshot_t) {
        .count = handle->count,
        .celebrating = handle->timer != NULL,
        .settings_open = handle->settings_open,
        .brightness = handle->settings.brightness,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

void app_hello_set_pref_handler(app_hello_handle_t handle, app_hello_pref_cb_t on_pref,
                                app_hello_activity_cb_t on_activity, void *ctx)
{
    if (!handle) {
        return;
    }
    handle->on_pref = on_pref;
    handle->on_activity = on_activity;
    handle->handler_ctx = ctx;
}

void app_hello_request_settings_toggle(app_hello_handle_t handle)
{
    if (handle) {
        handle->toggle_req = 1;
    }
}

void app_hello_settings_post_view(app_hello_handle_t handle,
                                  const app_vibe_settings_view_t *view)
{
    int slot;
    if (!handle || !view) {
        return;
    }
    slot = (handle->settings_published ^ 1) & 1;
    handle->settings_buf[slot] = *view;
    handle->settings_published = slot;
    handle->view_dirty = 1;
}

void app_hello_take_password(app_hello_handle_t handle, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!handle) {
        return;
    }
    memcpy(dst, handle->pending_pass, dst_len - 1);
    dst[dst_len - 1] = '\0';
    memset(handle->pending_pass, 0, sizeof(handle->pending_pass));
}

esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = *handle;
    if (!state) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(state->ui, NULL, NULL);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (state->ui_timer) {
        err = esp_gsp_timer_delete(state->ui, state->ui_timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
    }
    if (state->timer) {
        err = esp_gsp_timer_delete(state->ui, state->timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
    }
    free(state);
    *handle = NULL;
    return ESP_GSP_OK;
}
