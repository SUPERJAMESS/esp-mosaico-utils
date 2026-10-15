// SPDX-License-Identifier: Apache-2.0
#include "vibe_ui_state.h"
#include "vui_ids.h"
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
#define TEXT(s, name, value) vui_set_text((s)->ui, VUI_VIBE_BIND_##name, vibe_translate(s, value))
static void format(vibe_ui_t *s, uint16_t bind, const char *fmt, ...)
{
    char value[512]; va_list args; va_start(args, fmt);
    vsnprintf(value, sizeof(value), vibe_translate(s, fmt), args); va_end(args);
    (void)vui_set_text(s->ui, bind, value);
}
#define FORMAT(s, name, ...) format(s, VUI_VIBE_BIND_##name, __VA_ARGS__)
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
    (void)vui_list_set_total(s->ui, s->bundles, count);
    (void)vui_list_refresh(s->ui, s->bundles);
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
    if (next != s->page && (s->page == VIBE_RESET || s->page == VIBE_DELETE_CONFIRM)) {
        s->hold_tick = 0;
        s->hold_consumed = true;
        (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, 0);
    }
    if (s->page == VIBE_SHARED_EDITOR && next != s->page) {
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_VALUE, "");
        s->shared_reveal = false;
        /* Delete confirmation may be cancelled back to the editor. */
        if (next != VIBE_DELETE_CONFIRM) memset(s->shared_value, 0, sizeof(s->shared_value));
    }
    if (next != s->page && (s->page == VIBE_NAND || s->page == VIBE_SHARED))
        return commit_delete(s);
    return true;
}
static void project_open_confirm(vibe_ui_t *s)
{
    s->open_waiting = s->snapshot.idea_busy;
    s->open_unsaved = s->snapshot.development_available && !s->snapshot.development_saved;
    bool low = s->snapshot.device.battery_valid && s->snapshot.device.battery_percent <= 20 &&
               !s->snapshot.device.usb_connected && !s->snapshot.device.charging;
    TEXT(s, OPEN_WARNING, s->open_waiting ? (s->snapshot.nand_scanning ? "Scanning NAND..." : "Finishing previous request...")
                                         : low ? "Charge above 20% or connect USB." : s->open_unsaved ? "Unsaved development will be replaced." : "The device will restart.");
    TEXT(s, OPEN_ACCEPT_LABEL, s->open_waiting ? "Please wait" : s->open_unsaved ? "Discard and open" : "Open");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_TOGGLE_OPEN_CONFIRM_VISIBLE, !s->open_waiting && !s->open_unsaved);
}
static void accept_open(vibe_ui_t *s)
{
    /* Show the dialog immediately, but never admit a write using stale scan
     * results or consent given before the unsaved-development warning. */
    bool waiting = s->open_waiting, unsaved = s->open_unsaved;
    s->services.snapshot(s->services.ctx, &s->snapshot);
    project_open_confirm(s);
    if (waiting || s->open_waiting || unsaved != s->open_unsaved) return;
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
    if (!s->selected_bundle.code[0]) return;
    s->return_page = s->page;
    vibe_show(s, VIBE_OPEN_CONFIRM);
    /* Compact controls use the bundled font; arbitrary cloud names have
     * their own MIFF glyphs only on the details page. */
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)s->selected_bundle.release; *p; ++p)
        if (*p < 32 || *p > 126) { ascii = false; break; }
    if (ascii) TEXT(s, OPEN_NAME, s->selected_bundle.release);
    else FORMAT(s, OPEN_NAME, "Idea %s", s->selected_bundle.code);
    project_open_confirm(s);
    if (!s->open_waiting && !s->open_unsaved && !s->snapshot.settings.open_confirm) accept_open(s);
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
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_VALUE, value[0] ? value : vibe_translate(s, s->shared_edit_value ? "Value" : "Key"));
    bool existing = false;
    for (size_t i = 0; i < s->snapshot.settings.shared_count; ++i)
        existing |= !strcmp(s->shared_key, s->snapshot.settings.shared[i].key);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_DELETE_VISIBLE, existing);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_REVEAL_VISIBLE, s->shared_edit_value && secret(s->shared_key));
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
bool vibe_product_event(vibe_ui_t *s, const vui_event_t *e)
{
    if (s->gesture_consumed || s->tick < s->suppress_until || s->snapshot.settings.busy) return true;
    /* A rapid second tap at the same coordinates must not activate a newly
     * revealed control (for example Download becoming Cancel download). */
    if (!s->pointer_page_ready || s->pointer_page != s->page) return true;
    unsigned action = e->action_id;
    if (s->snapshot.updating && !s->snapshot.batch && action != VUI_VIBE_ACT_ID_CANCEL_DOWNLOAD) return true;
    switch (action) {
    case VUI_VIBE_ACT_ID_FIRMWARE_START:
        if (s->page == VIBE_FIRMWARE_NOTICE) {
            if (!s->snapshot.release_installable) {
                TEXT(s, FIRMWARE_READY, "Firmware file not published yet");
                break;
            }
            int err = vibe_command(s, VIBE_INSTALL_FIRMWARE, s->snapshot.release_version, NULL);
            if (err) vibe_error(s, "Firmware update failed", err);
            else vibe_ui_poll(s);
        }
        break;
    case VUI_VIBE_ACT_ID_NAV_DEVELOP:
        vibe_show(s, VIBE_DEVELOP); break;
    case VUI_VIBE_ACT_ID_NAV_MANAGE:
        vibe_show(s, VIBE_NAND);
        if (s->snapshot.nand_error && !s->snapshot.idea_busy)
            (void)vibe_command(s, VIBE_SCAN_NAND, NULL, NULL);
        break;
    case VUI_VIBE_ACT_ID_NAV_SETTINGS: (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 0); vibe_show(s, VIBE_SETTINGS); break;
    case VUI_VIBE_ACT_ID_NAV_WIFI: vibe_show(s, VIBE_WIFI); break;
    case VUI_VIBE_ACT_ID_NAV_DISPLAY: (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_DISPLAY_VIEWPORT, 0); vibe_show(s, VIBE_DISPLAY); break;
    case VUI_VIBE_ACT_ID_NAV_HELP: vibe_show(s, VIBE_HELP); break;
    case VUI_VIBE_ACT_ID_SHOW_GUIDE: s->return_page = s->page; vibe_show(s, VIBE_GUIDE); break;
    case VUI_VIBE_ACT_ID_SHOW_BOOT_TIP: vibe_show(s, VIBE_BOOT_TIP); break;
    case VUI_VIBE_ACT_ID_NAV_LANGUAGE: vibe_show(s, VIBE_LANGUAGE); break;
    case VUI_VIBE_ACT_ID_NAV_SHARED: vibe_show(s, VIBE_SHARED); break;
    case VUI_VIBE_ACT_ID_NAV_ABOUT: vibe_show(s, VIBE_ABOUT); break;
    case VUI_VIBE_ACT_ID_NAV_RESET:
        if (!s->snapshot.idea_busy && commit_delete(s)) vibe_show(s, VIBE_RESET);
        break;
    case VUI_VIBE_ACT_ID_PRODUCT_BACK:
        if (s->page == VIBE_WELCOME) break;
        if (s->page == VIBE_OPEN_CONFIRM || s->page == VIBE_GUIDE) vibe_show(s, s->return_page);
        else if (s->page == VIBE_DELETE_CONFIRM) {
            vibe_show(s, s->delete_code[0] ? VIBE_CONFIRM : VIBE_SHARED_EDITOR);
            if (s->page == VIBE_SHARED_EDITOR) project_shared(s);
        }
        else if (s->page == VIBE_SHARED_EDITOR) {
            memset(s->shared_value, 0, sizeof(s->shared_value));
            vibe_show(s, VIBE_SHARED);
        } else if (s->page == VIBE_GRID || s->page == VIBE_DEVELOP || s->page == VIBE_SETTINGS || s->page == VIBE_BOOT_TIP || s->page == VIBE_FIRMWARE_NOTICE)
            vibe_show(s, VIBE_HOME);
        else vibe_show(s, VIBE_SETTINGS);
        break;
    case VUI_VIBE_ACT_ID_BACK_WIFI:
        if (s->first_run) { s->first_run = false; vibe_show(s, VIBE_HOME); }
        else vibe_show(s, VIBE_SETTINGS);
        break;
    case VUI_VIBE_ACT_ID_SET_LANGUAGE: number_setting(s, VIBE_SETTINGS_LANGUAGE, e->arg); break;
    case VUI_VIBE_ACT_ID_BRIGHTNESS: number_setting(s, VIBE_SETTINGS_BRIGHTNESS, e->arg); break;
    case VUI_VIBE_ACT_ID_TIMEOUT: number_setting(s, VIBE_SETTINGS_TIMEOUT, e->arg); break;
    case VUI_VIBE_ACT_ID_TOGGLE_ROTATE: number_setting(s, VIBE_SETTINGS_ROTATE, !s->snapshot.settings.auto_rotate); break;
    case VUI_VIBE_ACT_ID_TOGGLE_HAPTICS: number_setting(s, VIBE_SETTINGS_HAPTICS, !s->snapshot.settings.haptics); break;
    case VUI_VIBE_ACT_ID_INSTALL:
        if (s->page == VIBE_CONFIRM) open_confirm(s);
        break;
    case VUI_VIBE_ACT_ID_RESULT_OPEN:
        if (s->page == VIBE_RESULT && !s->snapshot.update_failed && !s->result_cancelled) open_confirm(s);
        break;
    case VUI_VIBE_ACT_ID_RESULT_DONE:
        if (s->page == VIBE_RESULT)
            vibe_show(s, s->result_cancelled ? VIBE_HOME : VIBE_NAND);
        break;
    case VUI_VIBE_ACT_ID_OPEN_CELL: {
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
    case VUI_VIBE_ACT_ID_OPEN_ACCEPT:
        if (s->page == VIBE_OPEN_CONFIRM) accept_open(s);
        break;
    case VUI_VIBE_ACT_ID_TOGGLE_OPEN_CONFIRM:
        number_setting(s, VIBE_SETTINGS_OPEN_CONFIRM, !s->snapshot.settings.open_confirm);
        break;
    case VUI_VIBE_ACT_ID_DELETE_IDEA:
        if (s->page != VIBE_CONFIRM) break;
        if (s->selected_bundle.boot) { TEXT(s, RELEASE_PATH, "Cannot delete the current Idea"); break; }
        if (commit_delete(s)) {
            snprintf(s->delete_code, sizeof(s->delete_code), "%s", s->selected_bundle.code);
            TEXT(s, DELETE_CONFIRM_TITLE, "Delete Idea");
            vibe_show(s, VIBE_DELETE_CONFIRM);
        }
        break;
    case VUI_VIBE_ACT_ID_UNDO_DELETE:
        s->undo_until = 0; refresh_library(s); break;
    case VUI_VIBE_ACT_ID_UPDATE_ALL: {
        /* A scan owns the Ideas worker too; keep its progress visible instead
         * of turning an early tap into an operation-failed result. */
        if (s->snapshot.idea_busy && !s->snapshot.batch) break;
        if (s->page != VIBE_NAND || !commit_delete(s)) break;
        if (s->snapshot.batch) {
            int err = vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL);
            if (err) vibe_error(s, "Operation failed", err);
            else s->batch_cancel_pending = true;
            break;
        }
        memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
        int err = vibe_command(s, VIBE_UPDATE_ALL, NULL, NULL);
        if (err) vibe_error(s, "Operation failed", err);
        break;
    }
    case VUI_VIBE_ACT_ID_CANCEL_DOWNLOAD:
        if (s->snapshot.cancellable && !vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL)) {
            s->snapshot.cancellable = false;
            TEXT(s, UPDATE_TITLE, "Cancelling...");
            TEXT(s, UPDATE_DETAIL, "Please wait");
            (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_CANCEL_DOWNLOAD_VISIBLE, false);
        }
        break;
    case VUI_VIBE_ACT_ID_SHARED_PREV: if (s->shared_page) --s->shared_page; break;
    case VUI_VIBE_ACT_ID_SHARED_NEXT:
        if ((s->shared_page + 1) * 4 < s->snapshot.settings.shared_count) ++s->shared_page;
        break;
    case VUI_VIBE_ACT_ID_SHARED_ADD:
        memset(s->shared_key, 0, sizeof(s->shared_key)); memset(s->shared_value, 0, sizeof(s->shared_value));
        s->shared_edit_value = s->shared_reveal = s->shared_wait = false; vibe_show(s, VIBE_SHARED_EDITOR); project_shared(s); break;
    case VUI_VIBE_ACT_ID_SHARED_ROW: {
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
    case VUI_VIBE_ACT_ID_SHARED_REVEAL: s->shared_reveal = !s->shared_reveal; project_shared(s); break;
    case VUI_VIBE_ACT_ID_SHARED_SUBMIT: if (s->page == VIBE_SHARED_EDITOR) shared_submit(s); break;
    case VUI_VIBE_ACT_ID_SHARED_DELETE:
        if (s->page == VIBE_SHARED_EDITOR && s->shared_key[0] && commit_delete(s)) {
            snprintf(s->undo_key, sizeof(s->undo_key), "%s", s->shared_key);
            s->delete_code[0] = 0;
            TEXT(s, DELETE_CONFIRM_TITLE, "Shared information");
            vibe_show(s, VIBE_DELETE_CONFIRM);
        }
        break;
    case VUI_VIBE_ACT_ID_SHARED_KEYBOARD_KEY: {
        if (s->page != VIBE_SHARED_EDITOR || s->shared_wait) break;
        if (e->arg == 13) { shared_submit(s); break; }
        char *value = s->shared_edit_value ? s->shared_value : s->shared_key;
        size_t n = strlen(value), cap = s->shared_edit_value ? sizeof(s->shared_value) : sizeof(s->shared_key);
        if (e->arg == 8 && n) value[--n] = 0;
        else if (e->arg >= 32 && e->arg <= 126 && n + 1 < cap) { value[n] = (char)e->arg; value[n + 1] = 0; }
        project_shared(s); break;
    }
    case VUI_VIBE_ACT_ID_NONE: break;
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
    static const uint16_t home[] = {VUI_VIBE_BIND_HOME_PAGER_2_VISIBLE, VUI_VIBE_BIND_HOME_PAGER_3_VISIBLE, VUI_VIBE_BIND_HOME_PAGER_4_VISIBLE};
    static const uint16_t grid[] = {VUI_VIBE_BIND_GRID_PAGER_2_1_VISIBLE,
        VUI_VIBE_BIND_GRID_PAGER_3_1_VISIBLE, VUI_VIBE_BIND_GRID_PAGER_3_2_VISIBLE,
        VUI_VIBE_BIND_GRID_PAGER_4_1_VISIBLE, VUI_VIBE_BIND_GRID_PAGER_4_2_VISIBLE, VUI_VIBE_BIND_GRID_PAGER_4_3_VISIBLE};
    unsigned index = 0;
    for (unsigned total = 2; total <= 4; ++total) {
        (void)vui_set_visible(s->ui, home[total - 2], total == pages && !active);
        for (unsigned current = 1; current < total; ++current)
            (void)vui_set_visible(s->ui, grid[index++], total == pages && current == active);
    }
}
static void render_grid(vibe_ui_t *s)
{
    static const uint16_t icons[] = {VUI_VIBE_BIND_CELL_ICON_0, VUI_VIBE_BIND_CELL_ICON_1, VUI_VIBE_BIND_CELL_ICON_2, VUI_VIBE_BIND_CELL_ICON_3, VUI_VIBE_BIND_CELL_ICON_4, VUI_VIBE_BIND_CELL_ICON_5};
    static const uint16_t names[] = {VUI_VIBE_BIND_CELL_NAME_0, VUI_VIBE_BIND_CELL_NAME_1, VUI_VIBE_BIND_CELL_NAME_2, VUI_VIBE_BIND_CELL_NAME_3, VUI_VIBE_BIND_CELL_NAME_4, VUI_VIBE_BIND_CELL_NAME_5};
    static const uint16_t icon_objects[] = {VUI_VIBE_BIND_CELL_ICON_0_VISIBLE, VUI_VIBE_BIND_CELL_ICON_1_VISIBLE, VUI_VIBE_BIND_CELL_ICON_2_VISIBLE, VUI_VIBE_BIND_CELL_ICON_3_VISIBLE, VUI_VIBE_BIND_CELL_ICON_4_VISIBLE, VUI_VIBE_BIND_CELL_ICON_5_VISIBLE};
    static const uint16_t defaults[] = {VUI_VIBE_BIND_CELL_DEFAULT_0_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_1_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_2_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_3_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_4_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_5_VISIBLE};
    static const uint16_t current[] = {VUI_VIBE_BIND_CELL_CURRENT_0_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_1_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_2_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_3_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_4_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_5_VISIBLE};
    static const uint16_t cards[] = {VUI_VIBE_BIND_BUTTON_CELL_0_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_1_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_2_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_3_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_4_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_5_VISIBLE};
    static const uint16_t add[] = {VUI_VIBE_BIND_CELL_ADD_0_VISIBLE, VUI_VIBE_BIND_CELL_ADD_1_VISIBLE, VUI_VIBE_BIND_CELL_ADD_2_VISIBLE,
        VUI_VIBE_BIND_CELL_ADD_3_VISIBLE, VUI_VIBE_BIND_CELL_ADD_4_VISIBLE, VUI_VIBE_BIND_CELL_ADD_5_VISIBLE};
    size_t visible = vibe_bundle_count(s);
    if (s->grid_page > visible / 6) s->grid_page = (unsigned)visible / 6;
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_EMPTY_VISIBLE, !visible);
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
        (void)vui_set_text(s->ui, names[cell], name);
        size_t size = 0;
        void *data = idea && s->services.icon ? s->services.icon(s->services.ctx, idea->code, &size) : NULL;
        bool has_icon = data != NULL;
        if (data && vui_set_image_owned(s->ui, icons[cell], data, size) != VUI_OK) { free(data); has_icon = false; }
        (void)vui_set_visible(s->ui, icon_objects[cell], has_icon);
        (void)vui_set_visible(s->ui, defaults[cell], present && !has_icon);
        (void)vui_set_visible(s->ui, current[cell], idea && idea->boot);
        (void)vui_set_visible(s->ui, cards[cell], visible ? present || add_cell : cell == 1);
        (void)vui_set_visible(s->ui, add[cell], add_cell);
    }
    if (visible) FORMAT(s, GRID_TITLE_TOP, "Ideas %u/%u", s->grid_page + 1, (unsigned)visible / 6 + 1);
    else TEXT(s, GRID_TITLE_TOP, "");
    s->grid_generation = s->snapshot.nand_generation + 1;
}
void vibe_product_poll(vibe_ui_t *s)
{
    const vibe_snapshot_t *v = &s->snapshot;
    if (s->page == VIBE_OPEN_CONFIRM) project_open_confirm(s);
    TEXT(s, BATCH_BUTTON, v->batch ? "Cancel" : "Update all");
    TEXT(s, TOGGLE_OPEN_CONFIRM_LABEL, v->settings.open_confirm ? "Always ask before opening" : "Skip this prompt next time");
    if (v->update_terminal && !v->updating && s->haptic_revision != v->update_revision) {
        s->haptic_revision = v->update_revision;
        haptic(s, v->update_failed ? 5 : 4);
    }
    if (s->last_language != v->settings.language) {
        s->last_language = v->settings.language;
        s->grid_generation = 0;
        (void)vui_list_refresh(s->ui, s->bundles);
        for (size_t i = 0; i < sizeof(vibe_static_labels) / sizeof(vibe_static_labels[0]); ++i)
            (void)vui_set_text(s->ui, vibe_static_labels[i].bind, vibe_translate(s, vibe_static_labels[i].text));
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
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_CANCEL_DOWNLOAD_VISIBLE, v->cancellable);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_RESULT_OPEN_VISIBLE, !v->update_failed && !s->result_cancelled && s->selected_bundle.code[0]);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_UNDO_DELETE_VISIBLE, s->undo_until != 0);
    if (s->undo_until) FORMAT(s, UNDO_MESSAGE, "Deleted · %u s to undo", (s->undo_until > s->tick ? s->undo_until - s->tick : 0) / 20 + 1);
    else TEXT(s, UNDO_MESSAGE, "");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_LIBRARY_EMPTY_VISIBLE, s->page == VIBE_NAND && !vibe_bundle_count(s));
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_SHARED_EMPTY_VISIBLE, s->page == VIBE_SHARED && !v->settings.shared_count);
    if (s->page == VIBE_LANGUAGE || s->page == VIBE_WELCOME) {
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_LANGUAGE_EN_SELECTED_VISIBLE, v->settings.language == 1);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_LANGUAGE_ZH_SELECTED_VISIBLE, v->settings.language == 2);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_WELCOME_EN_SELECTED_VISIBLE, v->settings.language == 1);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_WELCOME_ZH_SELECTED_VISIBLE, v->settings.language == 2);
    }
    if (s->page == VIBE_SETTINGS) {
        TEXT(s, SETTINGS_TOP_WIFI_SUB, v->network == VIBE_NET_CONNECTED ? v->ssid : "Not connected");
        if (v->settings.screen_off_seconds) FORMAT(s, SETTINGS_TOP_DISPLAY_SUB, "Screen off after %u min", v->settings.screen_off_seconds / 60);
        else TEXT(s, SETTINGS_TOP_DISPLAY_SUB, "Screen stays on");
        TEXT(s, SETTINGS_TOP_LANGUAGE_SUB, v->settings.language == 2 ? "简体中文" : "English");
        FORMAT(s, SETTINGS_TOP_SHARED_SUB, "%u entries", (unsigned)v->settings.shared_count);
    }
    if (s->page == VIBE_NAND) {
        if (v->nand_scanning) TEXT(s, NAND_SCAN, "Scanning NAND...");
        else if (v->nand_error) FORMAT(s, NAND_SCAN, "NAND unavailable: 0x%x", (unsigned)v->nand_error);
        else FORMAT(s, NAND_SCAN, "%u Ideas · %llu.%u of %llu MB used", (unsigned)vibe_bundle_count(s),
               (unsigned long long)(v->storage_used / 1000000), (unsigned)(v->storage_used / 100000 % 10),
               (unsigned long long)(v->storage_total / 1000000));
        (void)vui_set_value(s->ui, VUI_VIBE_BIND_STORAGE_BAR, v->storage_total ? v->storage_used * 100 / v->storage_total : 0);
        unsigned updates = 0;
        for (size_t i = 0; i < v->bundle_count; ++i) updates += v->bundles[i].update_available;
        if (v->batch) updates = v->batch_total > v->batch_index ? v->batch_total - v->batch_index : 0;
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_MANAGE_UPDATE_BADGE_VISIBLE, updates != 0);
        FORMAT(s, MANAGE_UPDATE_COUNT, "%u", updates);
    }
    if (s->page == VIBE_BRIDGE) {
        static const uint16_t digits[] = {VUI_VIBE_BIND_CODE_DIGIT_0, VUI_VIBE_BIND_CODE_DIGIT_1, VUI_VIBE_BIND_CODE_DIGIT_2,
                                          VUI_VIBE_BIND_CODE_DIGIT_3, VUI_VIBE_BIND_CODE_DIGIT_4, VUI_VIBE_BIND_CODE_DIGIT_5};
        for (unsigned i = 0; i < 6; ++i) {
            char value[2] = {i < strlen(s->idea_code) ? s->idea_code[i] : ' ', 0};
            (void)vui_set_text(s->ui, digits[i], value);
        }
    }
    if (s->page == VIBE_HOME || s->page == VIBE_GRID) {
        FORMAT(s, HOME_VERSION, "%s", v->firmware_version);
        FORMAT(s, GRID_VERSION, "%s", v->firmware_version);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_HEADING_EN_VISIBLE, v->settings.language != 2);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_HEADING_ZH_VISIBLE, v->settings.language == 2);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_WIFI_VISIBLE, v->network == VIBE_NET_CONNECTED);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_USB_VISIBLE, v->device.usb_connected);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_MID_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 20 && v->device.battery_percent <= 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_LOW_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_CHARGING_VISIBLE, v->device.battery_valid && v->device.charging);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_WIFI_VISIBLE, v->network == VIBE_NET_CONNECTED);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_USB_VISIBLE, v->device.usb_connected);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_MID_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent > 20 && v->device.battery_percent <= 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_LOW_VISIBLE, v->device.battery_valid && !v->device.charging && v->device.battery_percent <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_CHARGING_VISIBLE, v->device.battery_valid && v->device.charging);
        char battery[24] = "";
        if (v->device.battery_valid) snprintf(battery, sizeof(battery), "%u%%%s", v->device.battery_percent, v->device.charging ? "+" : "");
        FORMAT(s, HOME_STATUS, "%s %s %s", v->network == VIBE_NET_CONNECTED ? "Wi-Fi" : "", v->device.usb_connected ? "USB" : "", battery);
    }
    if (s->page == VIBE_GRID) {
        if (s->grid_generation != v->nand_generation + 1) render_grid(s);
    } else if (s->page == VIBE_DEVELOP) {
        TEXT(s, DEVELOP_USB, !v->device.usb_connected ? "USB not connected" : v->usb_owner ? "USB connected · Agent ready" : "USB connected · waiting for Agent");
    } else if (s->page == VIBE_DISPLAY) {
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_ROTATE_0_0_VISIBLE, !v->settings.auto_rotate);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_ROTATE_0_1_VISIBLE, v->settings.auto_rotate);
        static const uint16_t brightness[] = {VUI_VIBE_BIND_BRIGHTNESS_SELECTED_0_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_1_VISIBLE,
            VUI_VIBE_BIND_BRIGHTNESS_SELECTED_2_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_3_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_4_VISIBLE};
        static const uint16_t timeout[] = {VUI_VIBE_BIND_TIMEOUT_SELECTED_0_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_1_VISIBLE,
            VUI_VIBE_BIND_TIMEOUT_SELECTED_2_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_3_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_4_VISIBLE};
        static const unsigned seconds[] = {60, 180, 300, 600, 0};
        for (unsigned i = 0; i < 5; ++i) {
            (void)vui_set_visible(s->ui, brightness[i], v->settings.brightness == (i + 1) * 20);
            (void)vui_set_visible(s->ui, timeout[i], v->settings.screen_off_seconds == seconds[i]);
        }
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HAPTICS_0_0_VISIBLE, !v->settings.haptics);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HAPTICS_0_1_VISIBLE, v->settings.haptics);
        TEXT(s, TOGGLE_ROTATE_LABEL, v->settings.auto_rotate ? "Auto rotate: on" : "Auto rotate: off");
        TEXT(s, TOGGLE_HAPTICS_LABEL, v->settings.haptics ? "Haptics: on" : "Haptics: off");
        FORMAT(s, DISPLAY_SUMMARY, "Brightness %u%% · timeout %u s", v->settings.brightness, v->settings.screen_off_seconds);
    } else if (s->page == VIBE_ABOUT) {
        FORMAT(s, ABOUT_VERSION, "%s", v->firmware_version);
        FORMAT(s, ABOUT_MAC, "%s", v->mac);
        FORMAT(s, ABOUT_STORAGE, "%llu / %llu MiB", (unsigned long long)(v->storage_used / 1048576), (unsigned long long)(v->storage_total / 1048576));
    } else if (s->page == VIBE_SHARED) {
        const uint16_t rows[] = {VUI_VIBE_BIND_SHARED_ROW_0_LABEL, VUI_VIBE_BIND_SHARED_ROW_1_LABEL, VUI_VIBE_BIND_SHARED_ROW_2_LABEL, VUI_VIBE_BIND_SHARED_ROW_3_LABEL};
        for (size_t r = 0; r < 4; ++r) {
            size_t i = s->shared_page * 4 + r;
            if (i < v->settings.shared_count) {
                const vibe_shared_entry_t *entry = &v->settings.shared[i];
                if (s->undo_until && s->undo_shared && !strcmp(s->undo_key, entry->key))
                    format(s, rows[r], "Undo · %u s", (s->undo_until > s->tick ? s->undo_until - s->tick : 0) / 20 + 1);
                else format(s, rows[r], "%s: %.24s", entry->key, secret(entry->key) ? "********" : entry->value);
            } else (void)vui_set_text(s->ui, rows[r], "");
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
static void pointer(vui_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    vibe_ui_t *s = ctx;
    s->activity_tick = s->tick;
    if (s->page == VIBE_SLEEP && pressed) { wake(s); s->pointer_down = pressed; return; }
    if (pressed && !s->pointer_down) { s->gesture_consumed = false; s->hold_consumed = false; s->pointer_page = s->page; s->pointer_page_ready = s->tick - s->page_enter_tick >= 3; s->pointer_x = x; s->pointer_y = y; haptic(s, 1); }
    if (pressed && s->pointer_down && (abs(x - s->pointer_x) > 24 || abs(y - s->pointer_y) > 24)) s->gesture_consumed = true;
    if (s->page == VIBE_RESET || s->page == VIBE_DELETE_CONFIRM) {
        bool inside = x >= 24 && x < 456 && y >= 348 && y < 444;
        bool started_inside = s->pointer_x >= 24 && s->pointer_x < 456 &&
                              s->pointer_y >= 348 && s->pointer_y < 444;
        if (pressed && inside && started_inside && s->pointer_page == s->page &&
            s->pointer_page_ready && !s->snapshot.settings.busy) {
            if (!s->hold_tick && !s->hold_consumed) s->hold_tick = s->tick + 1;
        } else {
            s->hold_tick = 0;
            (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, 0);
        }
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
    s->pointer_down = pressed;
}
void vibe_product_tick(vibe_ui_t *s)
{
    if (s->undo_until && s->tick >= s->undo_until) {
        if (!commit_delete(s)) { s->undo_until = 0; refresh_library(s); }
    }
    if (s->hold_tick && s->tick + 1 >= s->hold_tick) {
        unsigned elapsed = s->tick + 1 - s->hold_tick;
        if (s->page == VIBE_RESET) (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, elapsed * 100 / 60);
        else if (s->page == VIBE_DELETE_CONFIRM) FORMAT(s, DELETE_PROGRESS, "Hold · %u s", 3 - (elapsed < 60 ? elapsed / 20 : 3));
        if (elapsed >= 60) {
            s->hold_tick = 0;
            s->hold_consumed = true;
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
    (void)vui_set_pointer_observer(s->ui, pointer, s);
    s->first_run = s->snapshot.settings.language == 0;
    if (s->first_run) vibe_show(s, VIBE_WELCOME);
}
