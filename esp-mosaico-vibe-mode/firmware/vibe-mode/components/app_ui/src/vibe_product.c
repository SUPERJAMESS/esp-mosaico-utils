// SPDX-License-Identifier: Apache-2.0
#include "vibe_ui_state.h"
#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"
#include "vibe_locale.inc"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *vibe_translate(vibe_ui_t *s, const char *text)
{
    if (!text) return "";
    if (s->snapshot.settings.language == 2)
        for (size_t i = 0; i < sizeof(vibe_translations) / sizeof(vibe_translations[0]); ++i)
            if (!strcmp(text, vibe_translations[i].en)) return vibe_translations[i].zh;
    return text;
}
#define TEXT(s, name, value) esp_gsp_set_text((s)->ui, GSP_VIBE_BIND_##name, vibe_translate(s, value))
static void format(vibe_ui_t *s, uint16_t bind, const char *fmt, ...)
{
    char value[512]; va_list args; va_start(args, fmt);
    vsnprintf(value, sizeof(value), vibe_translate(s, fmt), args); va_end(args);
    (void)esp_gsp_set_text(s->ui, bind, value);
}
#define FORMAT(s, name, ...) format(s, GSP_VIBE_BIND_##name, __VA_ARGS__)
static void haptic(vibe_ui_t *s, unsigned pattern)
{
    if (s->snapshot.settings.haptics && s->services.haptic) s->services.haptic(s->services.ctx, pattern);
}
static int setting(vibe_ui_t *s, vibe_settings_command_t command, const char *key, const char *value)
{
    int err = s->services.setting ? s->services.setting(s->services.ctx, command, key, value) : 0x106;
    if (err) vibe_error(s, "Operation failed", err);
    return err;
}
static void number_setting(vibe_ui_t *s, vibe_settings_command_t command, unsigned n)
{
    char value[16]; snprintf(value, sizeof(value), "%u", n); (void)setting(s, command, NULL, value);
}
size_t vibe_bundle_index(const vibe_ui_t *s, size_t visible)
{
    size_t found = 0;
    for (size_t i = 0; i < s->snapshot.bundle_count; ++i) {
        if (s->undo_until && !s->undo_shared && !strcmp(s->snapshot.bundles[i].code, s->delete_code)) continue;
        if (found++ == visible) return i;
    }
    return s->snapshot.bundle_count;
}
size_t vibe_bundle_count(const vibe_ui_t *s)
{
    size_t count = 0;
    for (size_t i = 0; i < s->snapshot.bundle_count; ++i)
        if (!(s->undo_until && !s->undo_shared && !strcmp(s->snapshot.bundles[i].code, s->delete_code))) ++count;
    return count;
}
static void refresh_library(vibe_ui_t *s)
{
    size_t count = vibe_bundle_count(s);
    (void)esp_gsp_list_set_total(s->ui, s->bundles, count);
    (void)esp_gsp_list_refresh(s->ui, s->bundles);
    s->grid_generation = 0;
}
static bool commit_delete(vibe_ui_t *s)
{
    if (!s->undo_until) return true;
    s->undo_until = 0;
    int err = s->undo_shared ? setting(s, VIBE_SETTINGS_SHARED_DELETE, s->undo_key, NULL)
                            : vibe_command(s, VIBE_DELETE_IDEA, s->delete_code, NULL);
    if (err) { vibe_error(s, "Operation failed", err); return false; }
    s->undo_until = 0;
    refresh_library(s);
    return true;
}
bool vibe_product_leave(vibe_ui_t *s, vibe_page_t next)
{
    if (s->page == VIBE_SHARED_EDITOR && next != s->page) {
        (void)esp_gsp_set_text(s->ui, GSP_VIBE_BIND_SHARED_VALUE, "");
        s->shared_reveal = false;
        /* Delete confirmation may be cancelled back to the editor. */
        if (next != VIBE_DELETE_CONFIRM) memset(s->shared_value, 0, sizeof(s->shared_value));
    }
    if (next != s->page && (s->page == VIBE_NAND || s->page == VIBE_SHARED))
        return commit_delete(s);
    return true;
}
static void accept_open(vibe_ui_t *s)
{
    if (s->snapshot.device.battery_valid && s->snapshot.device.battery_percent <= 20 &&
        !s->snapshot.device.usb_connected && !s->snapshot.device.charging) {
        TEXT(s, OPEN_WARNING, "Charge above 20% or connect USB."); haptic(s, 3); return;
    }
    if (!commit_delete(s)) return;
    int err = vibe_command(s, VIBE_OPEN_IDEA, s->selected_bundle.code, NULL);
    if (err) vibe_error(s, "Operation failed", err);
    else { vibe_show(s, VIBE_UPDATE); TEXT(s, UPDATE_TITLE, "Setting boot application"); haptic(s, 2); }
}
static void open_confirm(vibe_ui_t *s)
{
    if (s->snapshot.idea_busy || !s->selected_bundle.code[0]) return;
    s->return_page = s->page;
    vibe_show(s, VIBE_OPEN_CONFIRM);
    (void)esp_gsp_set_text(s->ui, GSP_VIBE_BIND_OPEN_NAME, s->selected_bundle.release);
    bool unsaved = s->snapshot.development_available && !s->snapshot.development_saved;
    TEXT(s, OPEN_WARNING, unsaved ? "Unsaved development will be replaced." : "The device will restart.");
    TEXT(s, OPEN_ACCEPT_LABEL, unsaved ? "Discard and open" : "Open");
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_TOGGLE_OPEN_CONFIRM_VISIBLE, !unsaved);
    if (!unsaved && !s->snapshot.settings.open_confirm) accept_open(s);
}
static bool secret(const char *key)
{
    return strstr(key, "key") || strstr(key, "token") || strstr(key, "secret") || strstr(key, "pass") || strstr(key, "pwd");
}
static void project_shared(vibe_ui_t *s)
{
    TEXT(s, SHARED_FIELD, s->shared_edit_value ? "Every Idea can read this value." : "a-z, 0-9 and _ · up to 15 characters");
    TEXT(s, SHARED_EDITOR_HEADING, s->shared_edit_value ? "Value" : "New key");
    const char *value = s->shared_edit_value ? s->shared_value : s->shared_key;
    size_t len = strlen(value);
    char masked[40];
    if (s->shared_edit_value && secret(s->shared_key) && !s->shared_reveal) {
        memset(masked, '*', len < 32 ? len : 32); masked[len < 32 ? len : 32] = 0; value = masked;
    } else if (len > 32) value += len - 32;
    (void)esp_gsp_set_text(s->ui, GSP_VIBE_BIND_SHARED_VALUE, value[0] ? value : vibe_translate(s, s->shared_edit_value ? "Value" : "Key"));
    bool existing = false;
    for (size_t i = 0; i < s->snapshot.settings.shared_count; ++i)
        existing |= !strcmp(s->shared_key, s->snapshot.settings.shared[i].key);
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_SHARED_DELETE_VISIBLE, existing);
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_SHARED_REVEAL_VISIBLE, s->shared_edit_value && secret(s->shared_key));
    TEXT(s, SHARED_REVEAL_LABEL, s->shared_reveal ? "Hide" : "Show");
    TEXT(s, SHARED_SUBMIT_LABEL, s->shared_edit_value ? "Save" : "Next");
}
static void shared_submit(vibe_ui_t *s)
{
    if (!s->shared_edit_value) {
        size_t len = strlen(s->shared_key);
        if (!len || len > 15 || strspn(s->shared_key, "abcdefghijklmnopqrstuvwxyz0123456789_") != len) {
            TEXT(s, SHARED_FIELD, "Please use a-z, 0-9 and _"); return;
        }
        s->shared_edit_value = true; project_shared(s);
    } else if (!setting(s, VIBE_SETTINGS_SHARED_SET, s->shared_key, s->shared_value)) {
        s->shared_wait = true;
        TEXT(s, SHARED_FIELD, "Saving settings...");
    }
}
bool vibe_product_event(vibe_ui_t *s, const esp_gsp_event_t *e)
{
    if ((!s->pointer_dispatch && s->gesture_consumed && e->action_id == s->consumed_action) ||
        s->tick < s->suppress_until || s->snapshot.settings.busy) return true;
    /* Composite scroll view hit routing is owned by the pointer observer. */
    if ((s->page == VIBE_SETTINGS || s->page == VIBE_DISPLAY) && !s->pointer_dispatch) return true;
    unsigned action = e->action_id;
    if (s->snapshot.updating && !s->snapshot.batch && action != GSP_VIBE_ACT_ID_CANCEL_DOWNLOAD) return true;
    switch (action) {
    case GSP_VIBE_ACT_ID_NAV_DEVELOP:
        vibe_show(s, VIBE_DEVELOP); (void)vibe_command(s, VIBE_SCAN_NAND, NULL, NULL); break;
    case GSP_VIBE_ACT_ID_NAV_MANAGE:
        vibe_show(s, VIBE_NAND); (void)vibe_command(s, VIBE_SCAN_NAND, NULL, NULL); break;
    case GSP_VIBE_ACT_ID_NAV_SETTINGS: s->settings_scroll = 0; vibe_show(s, VIBE_SETTINGS); break;
    case GSP_VIBE_ACT_ID_NAV_WIFI: vibe_show(s, VIBE_WIFI); break;
    case GSP_VIBE_ACT_ID_NAV_DISPLAY: s->display_scroll = 0; vibe_show(s, VIBE_DISPLAY); break;
    case GSP_VIBE_ACT_ID_NAV_HELP: vibe_show(s, VIBE_HELP); break;
    case GSP_VIBE_ACT_ID_SHOW_GUIDE: s->return_page = s->page; vibe_show(s, VIBE_GUIDE); break;
    case GSP_VIBE_ACT_ID_SHOW_BOOT_TIP: vibe_show(s, VIBE_BOOT_TIP); break;
    case GSP_VIBE_ACT_ID_NAV_LANGUAGE: vibe_show(s, VIBE_LANGUAGE); break;
    case GSP_VIBE_ACT_ID_NAV_SHARED: vibe_show(s, VIBE_SHARED); break;
    case GSP_VIBE_ACT_ID_NAV_ABOUT: vibe_show(s, VIBE_ABOUT); break;
    case GSP_VIBE_ACT_ID_NAV_RESET:
        if (!s->snapshot.idea_busy && commit_delete(s)) vibe_show(s, VIBE_RESET);
        break;
    case GSP_VIBE_ACT_ID_PRODUCT_BACK:
        if (s->page == VIBE_WELCOME) break;
        if (s->page == VIBE_OPEN_CONFIRM || s->page == VIBE_GUIDE) vibe_show(s, s->return_page);
        else if (s->page == VIBE_DELETE_CONFIRM) {
            vibe_show(s, s->delete_code[0] ? VIBE_CONFIRM : VIBE_SHARED_EDITOR);
            if (s->page == VIBE_SHARED_EDITOR) project_shared(s);
        }
        else if (s->page == VIBE_SHARED_EDITOR) {
            memset(s->shared_value, 0, sizeof(s->shared_value));
            vibe_show(s, VIBE_SHARED);
        } else if (s->page == VIBE_GRID || s->page == VIBE_DEVELOP || s->page == VIBE_SETTINGS || s->page == VIBE_BOOT_TIP)
            vibe_show(s, VIBE_HOME);
        else vibe_show(s, VIBE_SETTINGS);
        break;
    case GSP_VIBE_ACT_ID_BACK_WIFI:
        if (s->first_run) { s->first_run = false; vibe_show(s, VIBE_HOME); }
        else vibe_show(s, VIBE_SETTINGS);
        break;
    case GSP_VIBE_ACT_ID_SET_LANGUAGE: number_setting(s, VIBE_SETTINGS_LANGUAGE, e->arg); break;
    case GSP_VIBE_ACT_ID_BRIGHTNESS: number_setting(s, VIBE_SETTINGS_BRIGHTNESS, e->arg); break;
    case GSP_VIBE_ACT_ID_TIMEOUT: number_setting(s, VIBE_SETTINGS_TIMEOUT, e->arg); break;
    case GSP_VIBE_ACT_ID_TOGGLE_ROTATE: number_setting(s, VIBE_SETTINGS_ROTATE, !s->snapshot.settings.auto_rotate); break;
    case GSP_VIBE_ACT_ID_TOGGLE_HAPTICS: number_setting(s, VIBE_SETTINGS_HAPTICS, !s->snapshot.settings.haptics); break;
    case GSP_VIBE_ACT_ID_INSTALL:
        if (s->page == VIBE_CONFIRM) open_confirm(s);
        break;
    case GSP_VIBE_ACT_ID_RESULT_OPEN:
        if (s->page == VIBE_RESULT && !s->snapshot.update_failed) open_confirm(s);
        break;
    case GSP_VIBE_ACT_ID_OPEN_CELL: {
        if (s->page != VIBE_GRID || e->arg >= 6) break;
        size_t visible = vibe_bundle_count(s);
        if (!visible) {
            /* Reuse the center cell's hit target; no overlapping empty-state button. */
            if (e->arg == 1) (void)vibe_ui_open_bridge(s);
            break;
        }
        size_t index = vibe_bundle_index(s, s->grid_page * 6 + e->arg);
        if (index == s->snapshot.bundle_count) {
            if (s->grid_page * 6 + e->arg == visible) (void)vibe_ui_open_bridge(s);
            break;
        }
        s->selected_bundle = s->snapshot.bundles[index]; open_confirm(s); break;
    }
    case GSP_VIBE_ACT_ID_OPEN_ACCEPT:
        if (s->page == VIBE_OPEN_CONFIRM) accept_open(s);
        break;
    case GSP_VIBE_ACT_ID_TOGGLE_OPEN_CONFIRM:
        number_setting(s, VIBE_SETTINGS_OPEN_CONFIRM, !s->snapshot.settings.open_confirm);
        break;
    case GSP_VIBE_ACT_ID_DELETE_IDEA:
        if (s->page != VIBE_CONFIRM) break;
        if (s->selected_bundle.boot) { TEXT(s, RELEASE_PATH, "Cannot delete the current Idea"); break; }
        if (commit_delete(s)) {
            snprintf(s->delete_code, sizeof(s->delete_code), "%s", s->selected_bundle.code);
            TEXT(s, DELETE_CONFIRM_TITLE, "Delete Idea");
            vibe_show(s, VIBE_DELETE_CONFIRM);
        }
        break;
    case GSP_VIBE_ACT_ID_UNDO_DELETE:
        s->undo_until = 0; refresh_library(s); break;
    case GSP_VIBE_ACT_ID_UPDATE_ALL: {
        if (s->page != VIBE_NAND || !commit_delete(s)) break;
        if (s->snapshot.batch) { (void)vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL); break; }
        memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
        int err = vibe_command(s, VIBE_UPDATE_ALL, NULL, NULL);
        if (err) vibe_error(s, "Operation failed", err);
        break;
    }
    case GSP_VIBE_ACT_ID_RUN_DEVELOPMENT:
        if (s->page == VIBE_DEVELOP && s->snapshot.development_available) {
            int err = vibe_command(s, VIBE_RUN_DEVELOPMENT, NULL, NULL);
            if (err) vibe_error(s, "Operation failed", err);
        }
        break;
    case GSP_VIBE_ACT_ID_SAVE_DEVELOPMENT: {
        if (s->page != VIBE_DEVELOP || !s->snapshot.development_available || s->snapshot.development_saved) break;
        memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
        int err = vibe_command(s, VIBE_SAVE_DEVELOPMENT, NULL, NULL);
        if (err) vibe_error(s, "Operation failed", err);
        else vibe_show(s, VIBE_UPDATE);
        break;
    }
    case GSP_VIBE_ACT_ID_CANCEL_DOWNLOAD:
        if (s->snapshot.cancellable) (void)vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL);
        break;
    case GSP_VIBE_ACT_ID_SHARED_PREV: if (s->shared_page) --s->shared_page; break;
    case GSP_VIBE_ACT_ID_SHARED_NEXT:
        if ((s->shared_page + 1) * 4 < s->snapshot.settings.shared_count) ++s->shared_page;
        break;
    case GSP_VIBE_ACT_ID_SHARED_ADD:
        memset(s->shared_key, 0, sizeof(s->shared_key)); memset(s->shared_value, 0, sizeof(s->shared_value));
        s->shared_edit_value = s->shared_reveal = s->shared_wait = false; vibe_show(s, VIBE_SHARED_EDITOR); project_shared(s); break;
    case GSP_VIBE_ACT_ID_SHARED_ROW: {
        size_t i = s->shared_page * 4 + e->arg;
        if (s->page != VIBE_SHARED || e->arg >= 4 || i >= s->snapshot.settings.shared_count) break;
        const vibe_shared_entry_t *entry = &s->snapshot.settings.shared[i];
        if (s->undo_until && s->undo_shared && !strcmp(entry->key, s->undo_key)) {
            s->undo_until = 0; break;
        }
        snprintf(s->shared_key, sizeof(s->shared_key), "%s", entry->key);
        snprintf(s->shared_value, sizeof(s->shared_value), "%s", entry->value);
        s->shared_edit_value = true; s->shared_reveal = s->shared_wait = false;
        vibe_show(s, VIBE_SHARED_EDITOR); project_shared(s); break;
    }
    case GSP_VIBE_ACT_ID_SHARED_REVEAL: s->shared_reveal = !s->shared_reveal; project_shared(s); break;
    case GSP_VIBE_ACT_ID_SHARED_SUBMIT: if (s->page == VIBE_SHARED_EDITOR) shared_submit(s); break;
    case GSP_VIBE_ACT_ID_SHARED_DELETE:
        if (s->page == VIBE_SHARED_EDITOR && s->shared_key[0] && commit_delete(s)) {
            snprintf(s->undo_key, sizeof(s->undo_key), "%s", s->shared_key);
            s->delete_code[0] = 0;
            TEXT(s, DELETE_CONFIRM_TITLE, "Shared information");
            vibe_show(s, VIBE_DELETE_CONFIRM);
        }
        break;
    case GSP_VIBE_ACT_ID_SHARED_KEYBOARD_KEY: {
        if (s->page != VIBE_SHARED_EDITOR || s->shared_wait) break;
        if (e->arg == 13) { shared_submit(s); break; }
        char *value = s->shared_edit_value ? s->shared_value : s->shared_key;
        size_t n = strlen(value), cap = s->shared_edit_value ? sizeof(s->shared_value) : sizeof(s->shared_key);
        if (e->arg == 8 && n) value[--n] = 0;
        else if (e->arg >= 32 && e->arg <= 126 && n + 1 < cap) { value[n] = (char)e->arg; value[n + 1] = 0; }
        project_shared(s); break;
    }
    case GSP_VIBE_ACT_ID_NONE: break;
    default: return false;
    }
    vibe_product_poll(s);
    return true;
}
static void render_pager(vibe_ui_t *s)
{
    const unsigned pages = (unsigned)vibe_bundle_count(s) / 6 + 2;
    const unsigned active = s->page == VIBE_HOME ? 0 : s->grid_page + 1;
    const unsigned key = pages * 16 + active;
    if (s->pager_state == key) return;
    s->pager_state = key;
    static const uint16_t home[] = {GSP_VIBE_BIND_HOME_PAGER_2_VISIBLE, GSP_VIBE_BIND_HOME_PAGER_3_VISIBLE, GSP_VIBE_BIND_HOME_PAGER_4_VISIBLE};
    static const uint16_t grid[] = {GSP_VIBE_BIND_GRID_PAGER_2_1_VISIBLE,
        GSP_VIBE_BIND_GRID_PAGER_3_1_VISIBLE, GSP_VIBE_BIND_GRID_PAGER_3_2_VISIBLE,
        GSP_VIBE_BIND_GRID_PAGER_4_1_VISIBLE, GSP_VIBE_BIND_GRID_PAGER_4_2_VISIBLE, GSP_VIBE_BIND_GRID_PAGER_4_3_VISIBLE};
    unsigned index = 0;
    for (unsigned total = 2; total <= 4; ++total) {
        (void)esp_gsp_set_visible(s->ui, home[total - 2], total == pages && !active);
        for (unsigned current = 1; current < total; ++current)
            (void)esp_gsp_set_visible(s->ui, grid[index++], total == pages && current == active);
    }
}
static void render_grid(vibe_ui_t *s)
{
    static const uint16_t icons[] = {GSP_VIBE_BIND_CELL_ICON_0, GSP_VIBE_BIND_CELL_ICON_1, GSP_VIBE_BIND_CELL_ICON_2, GSP_VIBE_BIND_CELL_ICON_3, GSP_VIBE_BIND_CELL_ICON_4, GSP_VIBE_BIND_CELL_ICON_5};
    static const uint16_t names[] = {GSP_VIBE_BIND_CELL_NAME_0, GSP_VIBE_BIND_CELL_NAME_1, GSP_VIBE_BIND_CELL_NAME_2, GSP_VIBE_BIND_CELL_NAME_3, GSP_VIBE_BIND_CELL_NAME_4, GSP_VIBE_BIND_CELL_NAME_5};
    static const uint16_t icon_objects[] = {GSP_VIBE_BIND_CELL_ICON_0_VISIBLE, GSP_VIBE_BIND_CELL_ICON_1_VISIBLE, GSP_VIBE_BIND_CELL_ICON_2_VISIBLE, GSP_VIBE_BIND_CELL_ICON_3_VISIBLE, GSP_VIBE_BIND_CELL_ICON_4_VISIBLE, GSP_VIBE_BIND_CELL_ICON_5_VISIBLE};
    static const uint16_t defaults[] = {GSP_VIBE_BIND_CELL_DEFAULT_0_VISIBLE, GSP_VIBE_BIND_CELL_DEFAULT_1_VISIBLE, GSP_VIBE_BIND_CELL_DEFAULT_2_VISIBLE, GSP_VIBE_BIND_CELL_DEFAULT_3_VISIBLE, GSP_VIBE_BIND_CELL_DEFAULT_4_VISIBLE, GSP_VIBE_BIND_CELL_DEFAULT_5_VISIBLE};
    static const uint16_t current[] = {GSP_VIBE_BIND_CELL_CURRENT_0_VISIBLE, GSP_VIBE_BIND_CELL_CURRENT_1_VISIBLE, GSP_VIBE_BIND_CELL_CURRENT_2_VISIBLE, GSP_VIBE_BIND_CELL_CURRENT_3_VISIBLE, GSP_VIBE_BIND_CELL_CURRENT_4_VISIBLE, GSP_VIBE_BIND_CELL_CURRENT_5_VISIBLE};
    static const uint16_t cards[] = {GSP_VIBE_BIND_BUTTON_CELL_0_VISIBLE, GSP_VIBE_BIND_BUTTON_CELL_1_VISIBLE, GSP_VIBE_BIND_BUTTON_CELL_2_VISIBLE, GSP_VIBE_BIND_BUTTON_CELL_3_VISIBLE, GSP_VIBE_BIND_BUTTON_CELL_4_VISIBLE, GSP_VIBE_BIND_BUTTON_CELL_5_VISIBLE};
    static const uint16_t add[] = {GSP_VIBE_BIND_CELL_ADD_0_VISIBLE, GSP_VIBE_BIND_CELL_ADD_1_VISIBLE, GSP_VIBE_BIND_CELL_ADD_2_VISIBLE,
        GSP_VIBE_BIND_CELL_ADD_3_VISIBLE, GSP_VIBE_BIND_CELL_ADD_4_VISIBLE, GSP_VIBE_BIND_CELL_ADD_5_VISIBLE};
    size_t visible = vibe_bundle_count(s);
    if (s->grid_page > visible / 6) s->grid_page = (unsigned)visible / 6;
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_EMPTY_VISIBLE, !visible);
    for (size_t cell = 0; cell < 6; ++cell) {
        size_t i = vibe_bundle_index(s, s->grid_page * 6 + cell);
        bool present = i < s->snapshot.bundle_count;
        const vibe_bundle_t *idea = present ? &s->snapshot.bundles[i] : NULL;
        char name[80] = "";
        bool add_cell = visible && s->grid_page * 6 + cell == visible;
        if (add_cell) snprintf(name, sizeof(name), "%s", vibe_translate(s, "Add Idea"));
        if (idea) {
            bool ascii = strspn(idea->release, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.,!?()") == strlen(idea->release);
            const char *title = ascii ? idea->release : idea->code;
            snprintf(name, sizeof(name), "%.8s%s", title, strlen(title) > 8 ? "..." : idea->update_available ? " ↑" : "");
        }
        (void)esp_gsp_set_text(s->ui, names[cell], name);
        size_t size = 0;
        void *data = idea && s->services.icon ? s->services.icon(s->services.ctx, idea->code, &size) : NULL;
        bool has_icon = data != NULL;
        if (data && esp_gsp_set_image_owned(s->ui, icons[cell], data, size) != ESP_GSP_OK) { free(data); has_icon = false; }
        (void)esp_gsp_set_visible(s->ui, icon_objects[cell], has_icon);
        (void)esp_gsp_set_visible(s->ui, defaults[cell], present && !has_icon);
        (void)esp_gsp_set_visible(s->ui, current[cell], idea && idea->boot);
        (void)esp_gsp_set_visible(s->ui, cards[cell], visible ? present || add_cell : cell == 1);
        (void)esp_gsp_set_visible(s->ui, add[cell], add_cell);
    }
    if (visible) FORMAT(s, GRID_TITLE_TOP, "Ideas %u/%u", s->grid_page + 1, (unsigned)visible / 6 + 1);
    else TEXT(s, GRID_TITLE_TOP, "");
    s->grid_generation = s->snapshot.nand_generation + 1;
}
void vibe_product_poll(vibe_ui_t *s)
{
    const vibe_snapshot_t *v = &s->snapshot;
    TEXT(s, BATCH_BUTTON, v->batch ? "Cancel" : "Update all");
    TEXT(s, TOGGLE_OPEN_CONFIRM_LABEL, v->settings.open_confirm ? "Always ask before opening" : "Skip this prompt next time");
    if (v->update_terminal && !v->updating && s->haptic_revision != v->update_revision) {
        s->haptic_revision = v->update_revision;
        haptic(s, v->update_failed ? 5 : 4);
    }
    if (s->last_language != v->settings.language) {
        s->last_language = v->settings.language;
        s->grid_generation = 0;
        (void)esp_gsp_list_refresh(s->ui, s->bundles);
        for (size_t i = 0; i < sizeof(vibe_static_labels) / sizeof(vibe_static_labels[0]); ++i)
            (void)esp_gsp_set_text(s->ui, vibe_static_labels[i].bind, vibe_translate(s, vibe_static_labels[i].text));
        if (s->first_run && v->settings.language) {
            vibe_show(s, VIBE_WIFI);
            TEXT(s, WIFI_NOTE, "Choose Wi-Fi or skip for now");
        }
    }
    if (s->settings_revision != v->settings.revision) {
        s->settings_revision = v->settings.revision;
        if (v->settings.error) { s->shared_wait = false; vibe_error(s, "Operation failed", v->settings.error); }
        else if (s->shared_wait && !v->settings.busy) {
            s->shared_wait = false; memset(s->shared_value, 0, sizeof(s->shared_value)); vibe_show(s, VIBE_SHARED);
        }
    }
    if (s->first_run && s->page == VIBE_WIFI && v->network == VIBE_NET_CONNECTED) {
        s->first_run = false; vibe_show(s, VIBE_HOME);
    }
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_CANCEL_DOWNLOAD_VISIBLE, v->cancellable);
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_RESULT_OPEN_VISIBLE, !v->update_failed && s->selected_bundle.code[0]);
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_BUTTON_UNDO_DELETE_VISIBLE, s->undo_until != 0);
    if (s->undo_until) FORMAT(s, UNDO_MESSAGE, "Deleted · %u s to undo", (s->undo_until > s->tick ? s->undo_until - s->tick : 0) / 20 + 1);
    else TEXT(s, UNDO_MESSAGE, "");
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_LIBRARY_EMPTY_VISIBLE, s->page == VIBE_NAND && !vibe_bundle_count(s));
    (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_SHARED_EMPTY_VISIBLE, s->page == VIBE_SHARED && !v->settings.shared_count);
    if (s->page == VIBE_LANGUAGE || s->page == VIBE_WELCOME) {
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_LANGUAGE_EN_SELECTED_VISIBLE, v->settings.language == 1);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_LANGUAGE_ZH_SELECTED_VISIBLE, v->settings.language == 2);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_WELCOME_EN_SELECTED_VISIBLE, v->settings.language == 1);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_WELCOME_ZH_SELECTED_VISIBLE, v->settings.language == 2);
    }
    if (s->page == VIBE_SETTINGS) {
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_SETTINGS_TOP_VISIBLE, !s->settings_scroll);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_SETTINGS_BOTTOM_VISIBLE, s->settings_scroll);
        TEXT(s, SETTINGS_TOP_WIFI_SUB, v->network == VIBE_NET_CONNECTED ? v->ssid : "Not connected");
        if (v->settings.screen_off_seconds) FORMAT(s, SETTINGS_TOP_DISPLAY_SUB, "Screen off after %u min", v->settings.screen_off_seconds / 60);
        else TEXT(s, SETTINGS_TOP_DISPLAY_SUB, "Screen stays on");
        TEXT(s, SETTINGS_TOP_LANGUAGE_SUB, v->settings.language == 2 ? "简体中文" : "English");
        FORMAT(s, SETTINGS_TOP_SHARED_SUB, "%u entries", (unsigned)v->settings.shared_count);
        FORMAT(s, SETTINGS_BOTTOM_SHARED_SUB, "%u entries", (unsigned)v->settings.shared_count);
    }
    if (s->page == VIBE_NAND) {
        if (v->nand_scanning) TEXT(s, NAND_SCAN, "Scanning NAND...");
        else if (v->nand_error) FORMAT(s, NAND_SCAN, "NAND unavailable: 0x%x", (unsigned)v->nand_error);
        else FORMAT(s, NAND_SCAN, "%u Ideas · %llu.%u of %llu MB used", (unsigned)vibe_bundle_count(s),
               (unsigned long long)(v->storage_used / 1000000), (unsigned)(v->storage_used / 100000 % 10),
               (unsigned long long)(v->storage_total / 1000000));
        (void)esp_gsp_set_value(s->ui, GSP_VIBE_BIND_STORAGE_BAR, v->storage_total ? v->storage_used * 100 / v->storage_total : 0);
        unsigned updates = 0;
        for (size_t i = 0; i < v->bundle_count; ++i) updates += v->bundles[i].update_available;
        if (v->batch) updates = v->batch_total > v->batch_index ? v->batch_total - v->batch_index : 0;
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_MANAGE_UPDATE_BADGE_VISIBLE, updates != 0);
        FORMAT(s, MANAGE_UPDATE_COUNT, "%u", updates);
    }
    if (s->page == VIBE_BRIDGE) {
        static const uint16_t digits[] = {GSP_VIBE_BIND_CODE_DIGIT_0, GSP_VIBE_BIND_CODE_DIGIT_1, GSP_VIBE_BIND_CODE_DIGIT_2,
                                          GSP_VIBE_BIND_CODE_DIGIT_3, GSP_VIBE_BIND_CODE_DIGIT_4, GSP_VIBE_BIND_CODE_DIGIT_5};
        for (unsigned i = 0; i < 6; ++i) {
            char value[2] = {i < strlen(s->idea_code) ? s->idea_code[i] : ' ', 0};
            (void)esp_gsp_set_text(s->ui, digits[i], value);
        }
    }
    if (s->page == VIBE_HOME || s->page == VIBE_GRID) {
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HOME_HEADING_EN_VISIBLE, v->settings.language != 2);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HOME_HEADING_ZH_VISIBLE, v->settings.language == 2);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_WIFI_VISIBLE, v->network == VIBE_NET_CONNECTED);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_USB_VISIBLE, v->device.usb_connected);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_BATTERY_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 60);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_BATTERY_MID_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 20 && v->device.battery_percent <= 60);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_BATTERY_LOW_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent <= 20);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_STATUS_BATTERY_CHARGING_VISIBLE, v->device.battery_valid && v->device.charging);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_WIFI_VISIBLE, v->network == VIBE_NET_CONNECTED);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_USB_VISIBLE, v->device.usb_connected);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_BATTERY_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 60);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_BATTERY_MID_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 20 && v->device.battery_percent <= 60);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_BATTERY_LOW_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent <= 20);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_GRID_STATUS_BATTERY_CHARGING_VISIBLE, v->device.battery_valid && v->device.charging);
        char battery[24] = "";
        if (v->device.battery_valid) snprintf(battery, sizeof(battery), "%u%%%s", v->device.battery_percent, v->device.charging ? "+" : "");
        FORMAT(s, HOME_STATUS, "%s %s %s", v->network == VIBE_NET_CONNECTED ? "Wi-Fi" : "", v->device.usb_connected ? "USB" : "", battery);
    }
    if (s->page == VIBE_GRID) {
        if (s->grid_generation != v->nand_generation + 1) render_grid(s);
    } else if (s->page == VIBE_DEVELOP) {
        TEXT(s, DEVELOP_USB, !v->device.usb_connected ? "Connect USB to your computer" : v->usb_owner ? "USB connected · Agent ready" : "USB connected · waiting for Agent");
        TEXT(s, DEVELOP_AGENT, "Ask your agent to install it here.");
        if (!v->development_available) TEXT(s, DEVELOP_APP, "No development Idea");
        else if (v->development_saved) TEXT(s, DEVELOP_APP, "Saved in My Ideas");
        else FORMAT(s, DEVELOP_APP, "%s · %s", v->development_name, v->development_version);
    } else if (s->page == VIBE_DISPLAY) {
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_DISPLAY_PART_0_VISIBLE, !s->display_scroll);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_DISPLAY_PART_1_VISIBLE, s->display_scroll);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_ROTATE_0_0_VISIBLE, !v->settings.auto_rotate);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_ROTATE_0_1_VISIBLE, v->settings.auto_rotate);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_ROTATE_1_0_VISIBLE, !v->settings.auto_rotate);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_ROTATE_1_1_VISIBLE, v->settings.auto_rotate);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HAPTICS_1_0_VISIBLE, !v->settings.haptics);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HAPTICS_1_1_VISIBLE, v->settings.haptics);
        static const uint16_t brightness[] = {GSP_VIBE_BIND_BRIGHTNESS_SELECTED_0_VISIBLE, GSP_VIBE_BIND_BRIGHTNESS_SELECTED_1_VISIBLE,
            GSP_VIBE_BIND_BRIGHTNESS_SELECTED_2_VISIBLE, GSP_VIBE_BIND_BRIGHTNESS_SELECTED_3_VISIBLE, GSP_VIBE_BIND_BRIGHTNESS_SELECTED_4_VISIBLE};
        static const uint16_t timeout[] = {GSP_VIBE_BIND_TIMEOUT_SELECTED_0_VISIBLE, GSP_VIBE_BIND_TIMEOUT_SELECTED_1_VISIBLE,
            GSP_VIBE_BIND_TIMEOUT_SELECTED_2_VISIBLE, GSP_VIBE_BIND_TIMEOUT_SELECTED_3_VISIBLE, GSP_VIBE_BIND_TIMEOUT_SELECTED_4_VISIBLE};
        static const unsigned seconds[] = {60, 180, 300, 600, 0};
        for (unsigned i = 0; i < 5; ++i) {
            (void)esp_gsp_set_visible(s->ui, brightness[i], v->settings.brightness == (i + 1) * 20);
            (void)esp_gsp_set_visible(s->ui, timeout[i], v->settings.screen_off_seconds == seconds[i]);
        }
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HAPTICS_0_0_VISIBLE, !v->settings.haptics);
        (void)esp_gsp_set_visible(s->ui, GSP_VIBE_BIND_HAPTICS_0_1_VISIBLE, v->settings.haptics);
        TEXT(s, TOGGLE_ROTATE_LABEL, v->settings.auto_rotate ? "Auto rotate: on" : "Auto rotate: off");
        TEXT(s, TOGGLE_HAPTICS_LABEL, v->settings.haptics ? "Haptics: on" : "Haptics: off");
        FORMAT(s, DISPLAY_SUMMARY, "Brightness %u%% · timeout %u s", v->settings.brightness, v->settings.screen_off_seconds);
    } else if (s->page == VIBE_ABOUT) {
        FORMAT(s, ABOUT_VERSION, "%s", v->firmware_version);
        FORMAT(s, ABOUT_MAC, "%s", v->mac);
        FORMAT(s, ABOUT_STORAGE, "%llu / %llu MiB", (unsigned long long)(v->storage_used / 1048576), (unsigned long long)(v->storage_total / 1048576));
    } else if (s->page == VIBE_SHARED) {
        const uint16_t rows[] = {GSP_VIBE_BIND_SHARED_ROW_0_LABEL, GSP_VIBE_BIND_SHARED_ROW_1_LABEL, GSP_VIBE_BIND_SHARED_ROW_2_LABEL, GSP_VIBE_BIND_SHARED_ROW_3_LABEL};
        for (size_t r = 0; r < 4; ++r) {
            size_t i = s->shared_page * 4 + r;
            if (i < v->settings.shared_count) {
                const vibe_shared_entry_t *entry = &v->settings.shared[i];
                if (s->undo_until && s->undo_shared && !strcmp(s->undo_key, entry->key))
                    format(s, rows[r], "Undo · %u s", (s->undo_until > s->tick ? s->undo_until - s->tick : 0) / 20 + 1);
                else format(s, rows[r], "%s: %.24s", entry->key, secret(entry->key) ? "********" : entry->value);
            } else (void)esp_gsp_set_text(s->ui, rows[r], "");
        }
    }
    if (s->page == VIBE_HOME || s->page == VIBE_GRID) render_pager(s);
}

static void sleep_now(vibe_ui_t *s)
{
    if (s->snapshot.idea_busy || s->snapshot.updating || s->snapshot.settings.busy || s->preview_active || !commit_delete(s)) return;
    s->wake_page = s->page;
    vibe_show(s, VIBE_SLEEP);
}
static void wake(vibe_ui_t *s)
{
    vibe_show(s, s->wake_page); s->activity_tick = s->tick; s->suppress_until = s->tick + 8;
    haptic(s, 1);
}
static void pointer(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    vibe_ui_t *s = ctx;
    s->activity_tick = s->tick;
    if (s->page == VIBE_SLEEP && pressed) { wake(s); s->pointer_down = pressed; return; }
    if (pressed && !s->pointer_down) { s->gesture_consumed = false; s->pointer_page = s->page; s->pointer_page_ready = s->tick - s->page_enter_tick >= 3; s->pointer_x = x; s->pointer_y = y; haptic(s, 1); }
    if (pressed && s->pointer_down && (abs(x - s->pointer_x) > 24 || abs(y - s->pointer_y) > 24)) s->gesture_consumed = true;
    if (s->page == VIBE_RESET || s->page == VIBE_DELETE_CONFIRM) {
        if (pressed && x >= 24 && x < 456 && y >= 348 && y < 444) {
            if (!s->hold_tick) s->hold_tick = s->tick + 1;
        } else s->hold_tick = 0;
    }
    if (!pressed && s->pointer_down && (s->page == VIBE_HOME || s->page == VIBE_GRID) && abs(y - s->pointer_y) < 80) {
        int dx = x - s->pointer_x;
        if (dx < -80) {
            bool moved = s->page == VIBE_HOME || (s->grid_page + 1) * 6 <= vibe_bundle_count(s);
            if (s->page == VIBE_HOME) { s->grid_page = 0; vibe_show(s, VIBE_GRID); }
            else if (moved) ++s->grid_page;
            if (moved) {
                s->grid_generation = 0; s->suppress_until = s->tick + 5;
            }
        } else if (dx > 80 && s->page == VIBE_GRID) {
            if (s->grid_page) --s->grid_page; else vibe_show(s, VIBE_HOME);
            s->grid_generation = 0; s->suppress_until = s->tick + 5;
        }
        vibe_product_poll(s);
    }
    if (!pressed && s->pointer_down && abs(x - s->pointer_x) < 80 && abs(y - s->pointer_y) > 45) {
        if (s->page == VIBE_SETTINGS) s->settings_scroll = y < s->pointer_y;
        else if (s->page == VIBE_DISPLAY) s->display_scroll = y < s->pointer_y;
        vibe_product_poll(s);
    }
    if (!pressed && s->pointer_down && !s->gesture_consumed && s->pointer_page_ready && s->pointer_page == s->page &&
        (s->page == VIBE_SETTINGS || s->page == VIBE_DISPLAY)) {
        esp_gsp_event_t action = {.action_id = GSP_VIBE_ACT_ID_NONE};
        if (x < 86 && y < 90) action.action_id = GSP_VIBE_ACT_ID_PRODUCT_BACK;
        else if (s->page == VIBE_SETTINGS && y >= 96 && y < 448 && (y - 96) % 88 < 80) {
            static const uint16_t actions[] = {GSP_VIBE_ACT_ID_NAV_WIFI, GSP_VIBE_ACT_ID_NAV_DISPLAY,
                GSP_VIBE_ACT_ID_NAV_LANGUAGE, GSP_VIBE_ACT_ID_NAV_SHARED, GSP_VIBE_ACT_ID_NAV_HELP,
                GSP_VIBE_ACT_ID_NAV_ABOUT, GSP_VIBE_ACT_ID_NAV_RESET};
            action.action_id = actions[(y - 96) / 88 + (s->settings_scroll ? 3 : 0)];
        } else if (s->page == VIBE_DISPLAY) {
            if (s->display_scroll) {
                if (y >= 96 && y < 176) action.action_id = GSP_VIBE_ACT_ID_TOGGLE_ROTATE;
                if (y >= 184 && y < 264) action.action_id = GSP_VIBE_ACT_ID_TOGGLE_HAPTICS;
            } else if (x >= 24 && x < 456 && (x - 24) % 88 < 80) {
                unsigned cell = (x - 24) / 88;
                if (y >= 124 && y < 196) { action.action_id = GSP_VIBE_ACT_ID_BRIGHTNESS; action.arg = (cell + 1) * 20; }
                if (y >= 240 && y < 312) {
                    static const unsigned timeouts[] = {60, 180, 300, 600, 0};
                    action.action_id = GSP_VIBE_ACT_ID_TIMEOUT; action.arg = timeouts[cell];
                }
                if (y >= 328 && y < 408) action.action_id = GSP_VIBE_ACT_ID_TOGGLE_ROTATE;
                if (y >= 416 && y < 480) action.action_id = GSP_VIBE_ACT_ID_TOGGLE_HAPTICS;
            }
        }
        s->pointer_dispatch = true;
        (void)vibe_product_event(s, &action);
        s->pointer_dispatch = false;
        s->consumed_action = action.action_id;
        s->gesture_consumed = true; /* Consume the renderer's synthetic click. */
    }
    s->pointer_down = pressed;
}
void vibe_product_tick(vibe_ui_t *s)
{
    if (s->undo_until && s->tick >= s->undo_until) {
        if (!commit_delete(s)) { s->undo_until = 0; refresh_library(s); }
    }
    if (s->hold_tick && s->tick + 1 >= s->hold_tick) {
        unsigned elapsed = s->tick + 1 - s->hold_tick;
        if (s->page == VIBE_RESET) FORMAT(s, RESET_PROGRESS, "Hold · %u s", 3 - (elapsed < 60 ? elapsed / 20 : 3));
        else if (s->page == VIBE_DELETE_CONFIRM) FORMAT(s, DELETE_PROGRESS, "Hold · %u s", 3 - (elapsed < 60 ? elapsed / 20 : 3));
        if (elapsed >= 60) {
            s->hold_tick = 0;
            if (s->page == VIBE_RESET && !s->snapshot.idea_busy && !s->snapshot.updating) (void)setting(s, VIBE_SETTINGS_RESET, NULL, NULL);
            else if (s->page == VIBE_DELETE_CONFIRM) {
                s->undo_shared = !s->delete_code[0];
                s->undo_until = s->tick + 80;
                memset(s->shared_value, 0, sizeof(s->shared_value));
                refresh_library(s); vibe_show(s, s->undo_shared ? VIBE_SHARED : VIBE_NAND);
                haptic(s, 2);
            }
        }
    }
    if (s->services.key) {
        unsigned key;
        while ((key = s->services.key(s->services.ctx)) != 0) {
            s->activity_tick = s->tick;
            if (key == 1) {
                if (s->page == VIBE_SLEEP) wake(s);
                else if (s->page == VIBE_OPEN_CONFIRM || s->page == VIBE_GUIDE) vibe_show(s, s->return_page);
                else if (s->page == VIBE_HOME || s->page == VIBE_GRID) sleep_now(s);
                else if (!s->snapshot.updating && !s->snapshot.idea_busy && !s->preview_active) vibe_show(s, VIBE_HOME);
            } else if (key == 3 && !s->snapshot.updating) {
                if (s->page == VIBE_SLEEP) wake(s);
                else { s->wake_page = s->page; vibe_show(s, VIBE_BOOT_TIP); }
            } else if (key == 4 && s->page == VIBE_BOOT_TIP) vibe_show(s, s->wake_page);
        }
    }
    if (s->motion_revision != s->snapshot.device.motion_revision) {
        s->motion_revision = s->snapshot.device.motion_revision;
        if (s->page == VIBE_SLEEP) wake(s);
    }
    unsigned timeout = s->snapshot.settings.screen_off_seconds;
    if (timeout && s->page != VIBE_SLEEP && s->page != VIBE_WELCOME &&
        s->tick - s->activity_tick >= timeout * 20) sleep_now(s);
    if (s->services.device_policy) s->services.device_policy(s->services.ctx, s->page == VIBE_SLEEP,
        !s->pointer_down && !s->hold_tick && s->page != VIBE_PASSWORD && s->page != VIBE_SHARED_EDITOR &&
        !s->snapshot.updating && s->page != VIBE_BOOT_TIP && s->page != VIBE_OPEN_CONFIRM);
}
void vibe_product_init(vibe_ui_t *s)
{
    (void)esp_gsp_set_pointer_observer(s->ui, pointer, s);
    s->first_run = s->snapshot.settings.language == 0;
    if (s->first_run) vibe_show(s, VIBE_WELCOME);
}
