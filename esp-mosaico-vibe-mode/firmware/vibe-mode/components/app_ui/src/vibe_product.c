// SPDX-License-Identifier: Apache-2.0
#include "vibe_ui_state.h"
#include "vui_ids.h"
#include "vui_input.h"
#include "vui_transition.h"
#include "factory_network_credentials.h"
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
    if (s->snapshot.device.haptics_available && s->snapshot.settings.haptics && s->services.haptic)
        s->services.haptic(s->services.ctx, pattern);
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
static bool shared_context(vibe_ui_t *s, vibe_page_t page)
{
    if (page == VIBE_SLEEP) page = s->wake_page;
    if (page == VIBE_BOOT_TIP) page = s->boot_page;
    if (page == VIBE_DELETE_CONFIRM && !s->delete_code[0]) page = s->return_page;
    return page == VIBE_SHARED_EDITOR || page == VIBE_SHARED_DETAIL || page == VIBE_SHARED_HELP;
}
bool vibe_product_leave(vibe_ui_t *s, vibe_page_t next)
{
    if (next == VIBE_HOME) {
        s->fix_return_active = false;
        if (s->wifi_return == VIBE_HELP) s->wifi_return = VIBE_SETTINGS;
    }
    if (next != s->page) vibe_product_cancel_pager(s);
    if (next != s->page && (s->page == VIBE_RESET || s->page == VIBE_DELETE_CONFIRM)) {
        s->hold_started_ms = 0;
        s->hold_consumed = true;
        (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, 0);
    }
    if (shared_context(s, s->page) && next != s->page) {
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_VALUE, "");
        s->shared_reveal = false;
        s->shared_detail_reveal = false;
        /* Temporary pages hide secrets without discarding the draft. Leaving
         * that context, including via a temporary page, clears it. */
        if (!shared_context(s, next)) memset(s->shared_value, 0, sizeof(s->shared_value));
    }
    if (next != s->page && (s->page == VIBE_NAND || s->page == VIBE_SHARED))
        return commit_delete(s);
    return true;
}
static void confirm_nand_format(vibe_ui_t *s)
{
    if (!s->snapshot.nand_needs_format || s->snapshot.idea_busy || s->snapshot.idea_ready ||
        s->snapshot.updating || s->snapshot.settings.busy) return;
    s->nand_prompt_dismissed = true;
    s->format_return = s->page;
    vibe_show(s, VIBE_FORMAT_NAND_CONFIRM);
}
static void project_open_name(vibe_ui_t *s)
{
    if (s->open_title_language != s->snapshot.settings.language) {
        s->open_title_ready = false;
        s->open_title_language = s->snapshot.settings.language;
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, false);
        (void)vui_set_image_owned(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, NULL, 0);
    }
    /* Metadata may finish loading or change locale while this dialog is open. */
    for (size_t i = 0; !s->run_development_pending && i < s->snapshot.bundle_count; ++i) {
        const vibe_bundle_t *item = &s->snapshot.bundles[i];
        if (!strcmp(item->code, s->selected_bundle.code) &&
            strcmp(item->release, s->selected_bundle.release)) {
            snprintf(s->selected_bundle.release, sizeof(s->selected_bundle.release), "%s", item->release);
            s->open_title_ready = false;
            (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, false);
            (void)vui_set_image_owned(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, NULL, 0);
        }
    }
    if (s->open_title_ready) return;
    const char *name = s->selected_bundle.release;
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p > 126) { ascii = false; break; }
    if (!ascii && s->services.title) {
        size_t size = 0;
        void *image = s->services.title(s->services.ctx, s->selected_bundle.code, &size);
        if (image) {
            if (vui_set_image_owned(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, image, size) == VUI_OK) {
                (void)vui_set_text(s->ui, VUI_VIBE_BIND_OPEN_NAME, "");
                (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, true);
                s->open_title_ready = true;
                return;
            }
            free(image);
        }
    }
    if (!*name) FORMAT(s, OPEN_NAME, "Idea %s", s->selected_bundle.code);
    else (void)vui_set_text(s->ui, VUI_VIBE_BIND_OPEN_NAME, ascii ? name : "...");
    s->open_title_ready = ascii && *name;
}
static void project_open_confirm(vibe_ui_t *s)
{
    project_open_name(s);
    s->open_waiting = s->snapshot.idea_busy;
    s->open_unsaved = !s->run_development_pending && s->snapshot.development_available && !s->snapshot.development_saved;
    bool low = s->snapshot.device.battery_valid && s->snapshot.device.battery_percent <= 20 &&
               !s->snapshot.device.usb_connected && !s->snapshot.device.charging;
    TEXT(s, OPEN_WARNING, s->open_waiting ? (s->snapshot.nand_scanning ? "Checking apps and storage..." : "Finishing previous request...")
                                         : low ? "Charge above 20% or connect USB." : s->open_unsaved ? "Unsaved development will be replaced." : "");
    TEXT(s, OPEN_ACCEPT_CAPTION, s->open_waiting ? "Please wait" : "Run");
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_OPEN_ACCEPT_LABEL, "");
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_TOGGLE_OPEN_CONFIRM_LABEL, "");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_CONFIRM_CHECKED_VISIBLE, !s->snapshot.settings.open_confirm);
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
    if (s->page == VIBE_OPEN_CONFIRM && !s->open_unsaved && s->snapshot.settings.open_confirm)
        number_setting(s, VIBE_SETTINGS_OPEN_CONFIRM, 2);
    if (s->run_development_pending) {
        s->run_development_pending = false;
        int err = vibe_command(s, VIBE_RUN_DEVELOPMENT, NULL, NULL);
        if (err) vibe_error(s, "Operation failed", err);
        else vibe_show(s, VIBE_HOME);
        return;
    }
    int err = vibe_command(s, VIBE_OPEN_IDEA, s->selected_bundle.code, NULL);
    if (err) vibe_error(s, "Operation failed", err);
    else { vibe_show(s, VIBE_UPDATE); TEXT(s, UPDATE_TITLE, "Setting boot application"); }
}
static void open_confirm(vibe_ui_t *s)
{
    if (!s->selected_bundle.code[0]) return;
    s->return_page = s->page;
    s->open_title_ready = false;
    s->open_title_language = s->snapshot.settings.language;
    (void)vui_set_image_owned(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, NULL, 0);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_NAME_PIXELS, false);
    vibe_show(s, VIBE_OPEN_CONFIRM);
    project_open_confirm(s);
    if (!s->open_waiting && !s->open_unsaved && !s->snapshot.settings.open_confirm) accept_open(s);
}
static bool secret(const char *key)
{
    if (!key) return false;
    char lower[64];
    size_t i = 0;
    for (; key[i] && i + 1 < sizeof(lower); ++i)
        lower[i] = (key[i] >= 'A' && key[i] <= 'Z') ? (char)(key[i] - 'A' + 'a') : key[i];
    lower[i] = 0;
    return strstr(lower, "key") || strstr(lower, "token") || strstr(lower, "secret") ||
           strstr(lower, "pass") || strstr(lower, "pwd");
}
static void project_shared(vibe_ui_t *s)
{
    TEXT(s, SHARED_FIELD, s->shared_edit_value ? "Every Idea can read this value." : "a-z, 0-9 and _ · up to 15 characters");
    TEXT(s, SHARED_EDITOR_HEADING, s->shared_edit_value ? "Value" : (s->shared_rename_mode ? "Key" : "New key"));
    const char *value = s->shared_edit_value ? s->shared_value : s->shared_key;
    size_t len = strlen(value);
    char masked[40];
    if (s->shared_edit_value && secret(s->shared_key) && !s->shared_reveal) {
        memset(masked, '*', len < 32 ? len : 32); masked[len < 32 ? len : 32] = 0; value = masked;
    } else if (len > 32) value += len - 32;
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_VALUE,
                       value[0] ? value : vibe_translate(s, "Enter text"));
    bool existing = false;
    const char *check_key = s->shared_rename_mode ? s->shared_orig_key : s->shared_key;
    for (size_t i = 0; i < s->snapshot.settings.shared_count; ++i)
        existing |= !strcmp(check_key, s->snapshot.settings.shared[i].key);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_DELETE_VISIBLE, existing);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_REVEAL_VISIBLE, s->shared_edit_value && secret(s->shared_key));
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_SHARED_EYE_ON_VISIBLE, s->shared_reveal);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_SHARED_EYE_OFF_VISIBLE, !s->shared_reveal);
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_REVEAL_LABEL, "");
    TEXT(s, SHARED_SUBMIT_LABEL, (s->shared_edit_value || s->shared_rename_mode) ? "Save" : "Next");
    const uint16_t spaces[] = {VUI_VIBE_OBJ_KEY_SHARED_KEYBOARD_SPACE_0, VUI_VIBE_OBJ_KEY_SHARED_KEYBOARD_SPACE_1,
        VUI_VIBE_OBJ_KEY_SHARED_KEYBOARD_SPACE_2, VUI_VIBE_OBJ_KEY_SHARED_KEYBOARD_SPACE_3};
    for (unsigned i = 0; i < 4; ++i) vui_set_text(s->ui, spaces[i], s->shared_edit_value ? "space" : "_");
}
static const char *const s_shared_sys_keys[7] = {
    "brightness", "haptics", "system_language", "wifi_ssid", "wifi_password", "screen_off", "auto_rotate"
};
static int shared_system_index(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < 7; ++i)
        if (!strcmp(key, s_shared_sys_keys[i])) return i;
    return -1;
}
static size_t shared_user_count(const vibe_settings_snapshot_t *settings)
{
    size_t n = 0;
    for (size_t i = 0; i < settings->shared_count; ++i)
        if (shared_system_index(settings->shared[i].key) < 0) ++n;
    return n;
}
static const vibe_shared_entry_t *shared_user_entry(const vibe_settings_snapshot_t *settings, size_t user_idx)
{
    size_t n = 0;
    for (size_t i = 0; i < settings->shared_count; ++i) {
        if (shared_system_index(settings->shared[i].key) < 0) {
            if (n == user_idx) return &settings->shared[i];
            ++n;
        }
    }
    return NULL;
}
static const char *shared_stored_value(const vibe_settings_snapshot_t *settings, const char *key)
{
    for (size_t i = 0; i < settings->shared_count; ++i)
        if (!strcmp(settings->shared[i].key, key)) return settings->shared[i].value;
    return NULL;
}
static void open_shared_system_settings(vibe_ui_t *s)
{
    s->shared_settings_return = true;
    if (!strcmp(s->shared_key, "wifi_ssid") || !strcmp(s->shared_key, "wifi_password")) {
        s->wifi_return = VIBE_SHARED;
        vibe_show(s, VIBE_WIFI);
    } else if (!strcmp(s->shared_key, "system_language")) {
        vibe_show(s, VIBE_LANGUAGE);
    } else {
        int scroll_y = (!strcmp(s->shared_key, "haptics") || !strcmp(s->shared_key, "auto_rotate")) ? 40 : 0;
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_DISPLAY_VIEWPORT, scroll_y);
        vibe_show(s, VIBE_DISPLAY);
    }
}
static void shared_submit(vibe_ui_t *s)
{
    if (!s->shared_edit_value) {
        size_t len = strlen(s->shared_key);
        if (!len || len > 15 || strspn(s->shared_key, "abcdefghijklmnopqrstuvwxyz0123456789_") != len) {
            TEXT(s, SHARED_FIELD, "Please use a-z, 0-9 and _"); return;
        }
        if (shared_system_index(s->shared_key) >= 0) {
            s->shared_value[0] = 0;
            s->shared_system_entry = true;
            s->shared_rename_mode = false;
            s->shared_reveal = s->shared_detail_reveal = s->shared_wait = false;
            vibe_show(s, VIBE_SHARED_DETAIL);
            return;
        }
        if (s->shared_rename_mode) {
            if (!strcmp(s->shared_key, s->shared_orig_key)) {
                s->shared_rename_mode = false;
                vibe_show(s, VIBE_SHARED_DETAIL);
                return;
            }
            if (shared_stored_value(&s->snapshot.settings, s->shared_key)) {
                TEXT(s, SHARED_FIELD, "Please use a-z, 0-9 and _");
                return;
            }
            if (!setting(s, VIBE_SETTINGS_SHARED_RENAME, s->shared_orig_key, s->shared_key)) {
                s->shared_wait = true;
                TEXT(s, SHARED_FIELD, "Saving settings...");
            }
            return;
        }
        s->shared_edit_value = true; project_shared(s);
    } else if (!setting(s, VIBE_SETTINGS_SHARED_SET, s->shared_key, s->shared_value)) {
        s->shared_wait = true;
        TEXT(s, SHARED_FIELD, "Saving settings...");
    }
}
static void project_help(vibe_ui_t *s)
{
    static const char *const titles[] = {
        "Getting started", "Develop Ideas", "Idea IDs & Sharing", "Troubleshooting", "Return to Vibe Mode", "Wi-Fi Connection", "Storage Is Full"
    };
    static const char *const steps[][3] = {
        {
            "Plug in USB and ask your agent to build an Idea.",
            "Or download one: open Manage, tap +, type its ID.",
            "Power off. Hold AI and power on.\nRelease AI in Vibe Mode.",
        },
        {
            "Open the Vibe-coding guide and\nfollow it in your AI agent.",
            "Plug in USB. The agent installs the\nIdea over the cable.",
            "Check Develop for USB and agent status.",
        },
        {
            "Every Idea has a 6-digit ID shown\non Mosaico Ideas.",
            "Open Manage, tap +, type the ID\nand press Go.",
            "Share your own Idea to get an ID\nfor friends.",
        },
        {"", "", ""},
        {
            "Power off the device.",
            "Hold AI and turn the device on.",
            "Release AI when Vibe Mode appears.",
        },
        {"Open Wi-Fi Settings and check\nthat Wi-Fi is on.", "Select your network and enter\nthe correct password.", "If connection fails, check the\npassword and scan again."},
        {"Open Manage Storage to check\nyour Ideas and storage usage.", "Delete an Idea you no longer need\nto free space.", "The current Idea cannot be deleted.\nRun another Idea first."},
    };
    unsigned idx = s->help_topic_index < 7 ? s->help_topic_index : 0;
    TEXT(s, HELP_HEADING, s->help_mode == 0 ? "Help" : s->help_mode == 2 ? "Troubleshooting" : titles[idx]);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_ROOT_LAYER_VISIBLE, s->help_mode == 0);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_STEPS_LAYER_VISIBLE, s->help_mode == 1);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_FIX_LAYER_VISIBLE, s->help_mode == 2);
    TEXT(s, HELP_QR_CAPTION, idx == 5 ? "Wi-Fi Settings" : idx == 6 ? "Manage Storage" : "Full guide on your phone");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_ACTION_QR_VISIBLE, idx < 5);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_ACTION_WIFI_VISIBLE, idx == 5);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HELP_ACTION_STORAGE_VISIBLE, idx == 6);
    if (s->help_mode == 1) {
        TEXT(s, HELP_STEP_BODY_1, steps[idx][0]);
        TEXT(s, HELP_STEP_BODY_2, steps[idx][1]);
        TEXT(s, HELP_STEP_BODY_3, steps[idx][2]);
    }
}
static void update_nav_badge(vibe_ui_t *s);
static const char *pointer_key(vibe_ui_t *s, const vui_event_t *e)
{
    if (e->action_id == VUI_VIBE_ACT_ID_SELECT_NETWORKS && s->page == VIBE_WIFI &&
        e->list == s->networks && e->item < s->snapshot.ap_count)
        return s->snapshot.aps[e->item].ssid;
    if (e->action_id == VUI_VIBE_ACT_ID_SELECT_BUNDLES && s->page == VIBE_NAND && e->list == s->bundles) {
        size_t index = vibe_bundle_index(s, e->item);
        return index < s->snapshot.bundle_count ? s->snapshot.bundles[index].code : "";
    }
    if (e->action_id == VUI_VIBE_ACT_ID_OPEN_CELL && s->page == VIBE_GRID && e->arg < 6) {
        size_t visible = vibe_bundle_count(s);
        if (!visible) return e->arg == 1 ? "+add" : "";
        size_t slot = s->grid_page * 6 + e->arg;
        if (slot == visible) return "+add";
        size_t index = vibe_bundle_index(s, slot);
        return index < s->snapshot.bundle_count ? s->snapshot.bundles[index].code : "";
    }
    if (e->action_id == VUI_VIBE_ACT_ID_SHARED_ROW && s->page == VIBE_SHARED && e->arg < 23) {
        size_t user_cnt = shared_user_count(&s->snapshot.settings);
        size_t index = e->arg;
        if (index < user_cnt) {
            const vibe_shared_entry_t *entry = shared_user_entry(&s->snapshot.settings, index);
            return entry ? entry->key : "";
        }
        size_t sys = index - user_cnt;
        return sys < 7 ? s_shared_sys_keys[sys] : "";
    }
    return NULL;
}
static bool pointer_actionable(vibe_ui_t *s, const vui_event_t *e, const char *key);
bool vibe_product_event(vibe_ui_t *s, const vui_event_t *e)
{
    if (s->gesture_consumed || vui_now_ms(s->ui) < s->suppress_until || s->snapshot.settings.busy) return true;
    /* A rapid second tap at the same coordinates must not activate a newly
     * revealed control (for example Download becoming Cancel download). */
    if (!s->pointer_page_ready || s->pointer_page != s->page) return true;
    /* Row indices can be reused by an asynchronous scan while the finger is
     * down. Content-only refreshes keep the key and remain tappable. */
    const char *key = pointer_key(s, e);
    if (key && strcmp(key, s->pointer_key)) return true;
    if (key && e->action_id == VUI_VIBE_ACT_ID_SELECT_NETWORKS &&
        s->pointer_network_open != s->snapshot.aps[e->item].open) return true;
    unsigned action = e->action_id;
    if (s->snapshot.updating && !s->snapshot.batch && action != VUI_VIBE_ACT_ID_CANCEL_DOWNLOAD) return true;
    switch (action) {
    case VUI_VIBE_ACT_ID_HOME_IDEAS:
        s->grid_page = 0; vibe_show(s, VIBE_GRID);
        s->nav_badge_until = vui_now_ms(s->ui) + 2000;
        update_nav_badge(s);
        break;
    case VUI_VIBE_ACT_ID_HOME_WIFI:
        if (s->snapshot.network != VIBE_NET_CONNECTED) { s->wifi_return = VIBE_HOME; vibe_show(s, VIBE_WIFI); }
        break;
    case VUI_VIBE_ACT_ID_LICENSES:
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_LICENSES_VIEWPORT, 0);
        vibe_show(s, VIBE_LICENSES); break;
    case VUI_VIBE_ACT_ID_SHARED_HELP:
        if (s->page == VIBE_SHARED_DETAIL) {
            if (s->shared_system_entry) {
                open_shared_system_settings(s);
            } else {
                snprintf(s->shared_orig_key, sizeof(s->shared_orig_key), "%s", s->shared_key);
                s->shared_rename_mode = true;
                s->shared_edit_value = s->shared_reveal = s->shared_wait = false;
                vibe_show(s, VIBE_SHARED_EDITOR);
                project_shared(s);
            }
            break;
        }
        s->return_page = s->page; vibe_show(s, VIBE_SHARED_HELP); break;
    case VUI_VIBE_ACT_ID_SHARED_DETAIL_REVEAL:
        if (s->page == VIBE_SHARED_DETAIL) s->shared_detail_reveal = !s->shared_detail_reveal;
        break;
    case VUI_VIBE_ACT_ID_SHARED_EDIT:
        if (s->shared_system_entry) {
            open_shared_system_settings(s);
            break;
        }
        s->shared_rename_mode = false;
        s->shared_edit_value = true;
        vibe_show(s, VIBE_SHARED_EDITOR); project_shared(s); break;
    case VUI_VIBE_ACT_ID_DOWNLOAD_HELP:
        s->guide_topic = 0; s->return_page = s->page; vibe_show(s, VIBE_GUIDE); break;
    case VUI_VIBE_ACT_ID_RUN_DEVELOPMENT:
        if (s->page != VIBE_DEVELOP) break;
        if (!s->snapshot.development_available) TEXT(s, DEVELOP_FEEDBACK, "No development build");
        else if (s->snapshot.idea_busy) TEXT(s, DEVELOP_FEEDBACK, "Finishing previous request...");
        else {
            s->run_development_pending = true;
            snprintf(s->selected_bundle.code, sizeof(s->selected_bundle.code), "dev");
            snprintf(s->selected_bundle.release, sizeof(s->selected_bundle.release), "%s",
                     s->snapshot.development_name[0] ? s->snapshot.development_name : "Development Idea");
            open_confirm(s);
        }
        break;
    case VUI_VIBE_ACT_ID_SAVE_DEVELOPMENT:
        if (s->page != VIBE_DEVELOP) break;
        if (!s->snapshot.development_available) TEXT(s, DEVELOP_FEEDBACK, "No development build to save");
        else if (s->snapshot.development_saved) TEXT(s, DEVELOP_FEEDBACK, "Already saved in My Ideas");
        else if (s->snapshot.idea_busy) TEXT(s, DEVELOP_FEEDBACK, "Finishing previous request...");
        else {
            memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
            int err = vibe_command(s, VIBE_SAVE_DEVELOPMENT, NULL, NULL);
            if (err) vibe_error(s, "Operation failed", err);
            else vibe_show(s, VIBE_UPDATE);
        }
        break;
    case VUI_VIBE_ACT_ID_FIRMWARE_CHECK:
        if (s->page != VIBE_ABOUT || s->firmware_check == CHECK_PENDING) break;
        if (s->snapshot.network != VIBE_NET_CONNECTED) {
            s->firmware_check = CHECK_OFFLINE;
            break;
        }
        s->firmware_check_generation = s->snapshot.release_generation;
        s->firmware_check = vibe_command(s, VIBE_CHECK_FIRMWARE, NULL, NULL) ? CHECK_FAILED : CHECK_PENDING;
        break;
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
        s->nand_prompt_dismissed = false;
        vibe_show(s, VIBE_NAND);
        if (s->snapshot.nand_error && !s->snapshot.nand_needs_format && !s->snapshot.idea_busy)
            (void)vibe_command(s, VIBE_SCAN_NAND, NULL, NULL);
        break;
    case VUI_VIBE_ACT_ID_NAV_SETTINGS: (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT, 0); vibe_show(s, VIBE_SETTINGS); break;
    case VUI_VIBE_ACT_ID_NAV_WIFI: s->wifi_return = VIBE_SETTINGS; vibe_show(s, VIBE_WIFI); break;
    case VUI_VIBE_ACT_ID_NAV_DISPLAY: (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_DISPLAY_VIEWPORT, 0); vibe_show(s, VIBE_DISPLAY); break;
    case VUI_VIBE_ACT_ID_NAV_HELP:
        s->help_mode = 0; s->fix_return_active = false;
        vibe_show(s, VIBE_HELP); project_help(s); break;
    case VUI_VIBE_ACT_ID_HELP_TOPIC_0:
    case VUI_VIBE_ACT_ID_HELP_TOPIC_1:
    case VUI_VIBE_ACT_ID_HELP_TOPIC_2:
        s->help_topic_index = action == VUI_VIBE_ACT_ID_HELP_TOPIC_0 ? 0
                            : action == VUI_VIBE_ACT_ID_HELP_TOPIC_1 ? 1 : 2;
        s->help_mode = 1; project_help(s); break;
    case VUI_VIBE_ACT_ID_HELP_TOPIC_3:
        s->help_topic_index = 3; s->help_mode = 2; project_help(s); break;
    case VUI_VIBE_ACT_ID_HELP_OPEN_QR:
        if (s->help_mode == 1 && s->help_topic_index == 5) {
            s->fix_return_active = true; s->wifi_return = VIBE_HELP;
            vibe_show(s, VIBE_WIFI); break;
        }
        if (s->help_mode == 1 && s->help_topic_index == 6) {
            s->fix_return_active = true; s->nand_prompt_dismissed = false;
            vibe_show(s, VIBE_NAND); break;
        }
        s->guide_topic = s->help_topic_index == 0 ? 1
                       : s->help_topic_index == 1 ? 0
                       : s->help_topic_index == 2 ? 2 : 3;
        s->return_page = VIBE_HELP; vibe_show(s, VIBE_GUIDE); break;
    case VUI_VIBE_ACT_ID_FIX_OPEN_QR:
        s->guide_topic = 3; s->return_page = VIBE_HELP; vibe_show(s, VIBE_GUIDE); break;
    case VUI_VIBE_ACT_ID_FIX_WIFI:
        s->help_topic_index = 5; s->help_mode = 1; project_help(s); break;
    case VUI_VIBE_ACT_ID_FIX_STORAGE:
        s->help_topic_index = 6; s->help_mode = 1; project_help(s); break;
    case VUI_VIBE_ACT_ID_FIX_RETURN:
        s->help_topic_index = 4; s->help_mode = 1; project_help(s); break;
    case VUI_VIBE_ACT_ID_SHOW_GUIDE: s->guide_topic = 0; s->return_page = s->page; vibe_show(s, VIBE_GUIDE); break;
    case VUI_VIBE_ACT_ID_NAV_LANGUAGE: s->shared_settings_return = false; vibe_show(s, VIBE_LANGUAGE); break;
    case VUI_VIBE_ACT_ID_NAV_SHARED:
        s->shared_page = 0;
        s->last_shared_user_count = (unsigned)shared_user_count(&s->snapshot.settings);
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT, 0);
        vibe_show(s, VIBE_SHARED);
        break;
    case VUI_VIBE_ACT_ID_NAV_ABOUT:
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_ABOUT_VIEWPORT, 0);
        s->firmware_check = CHECK_IDLE;
        vibe_show(s, VIBE_ABOUT);
        break;
    case VUI_VIBE_ACT_ID_NAV_RESET:
        if (!s->snapshot.idea_busy && commit_delete(s)) vibe_show(s, VIBE_RESET);
        break;
    case VUI_VIBE_ACT_ID_PRODUCT_BACK:
        if (s->page == VIBE_WELCOME) break;
        if (s->page == VIBE_HELP && s->help_mode != 0) {
            if (s->help_mode == 1 && s->help_topic_index >= 4) s->help_mode = 2;
            else s->help_mode = 0;
            project_help(s);
            break;
        }
        if (s->page == VIBE_FORMAT_NAND_CONFIRM) vibe_show(s, s->format_return);
        else if (s->page == VIBE_UPDATE_CONFIRM) vibe_show(s, VIBE_NAND);
        else if (s->page == VIBE_OPEN_CONFIRM || s->page == VIBE_GUIDE || s->page == VIBE_SHARED_HELP) vibe_show(s, s->return_page);
        else if (s->page == VIBE_LICENSES) vibe_show(s, VIBE_ABOUT);
        else if (s->page == VIBE_SHARED_DETAIL) vibe_show(s, VIBE_SHARED);
        else if (s->page == VIBE_DELETE_CONFIRM) {
            vibe_show(s, s->delete_code[0] ? VIBE_CONFIRM : s->return_page);
            if (s->page == VIBE_SHARED_EDITOR) project_shared(s);
        }
        else if (s->page == VIBE_SHARED_EDITOR) {
            if (s->shared_rename_mode) {
                snprintf(s->shared_key, sizeof(s->shared_key), "%s", s->shared_orig_key);
                s->shared_rename_mode = false;
                s->shared_edit_value = true;
                vibe_show(s, VIBE_SHARED_DETAIL);
            } else {
                memset(s->shared_value, 0, sizeof(s->shared_value));
                vibe_show(s, VIBE_SHARED);
            }
        } else if (s->page == VIBE_GRID || s->page == VIBE_DEVELOP || s->page == VIBE_SETTINGS || s->page == VIBE_BOOT_TIP || s->page == VIBE_FIRMWARE_NOTICE)
            vibe_show(s, VIBE_HOME);
        else if ((s->page == VIBE_LANGUAGE || s->page == VIBE_DISPLAY) && s->shared_settings_return) {
            s->shared_settings_return = false;
            vibe_show(s, VIBE_SHARED);
        }
        else vibe_show(s, VIBE_SETTINGS);
        break;
    case VUI_VIBE_ACT_ID_BACK_WIFI:
        if (s->first_run) { s->first_run = false; vibe_show(s, VIBE_HOME); }
        else if (s->download_pending) vibe_show(s, s->download_return);
        else if (s->fix_return_active) { s->fix_return_active = false; s->help_mode = 1; vibe_show(s, VIBE_HELP); project_help(s); }
        else vibe_show(s, s->wifi_return);
        break;
    case VUI_VIBE_ACT_ID_SET_LANGUAGE: number_setting(s, VIBE_SETTINGS_LANGUAGE, e->arg); break;
    case VUI_VIBE_ACT_ID_BRIGHTNESS: number_setting(s, VIBE_SETTINGS_BRIGHTNESS, e->arg); break;
    case VUI_VIBE_ACT_ID_TIMEOUT: number_setting(s, VIBE_SETTINGS_TIMEOUT, e->arg); break;
    case VUI_VIBE_ACT_ID_TOGGLE_ROTATE:
        if (s->snapshot.device.imu_available) number_setting(s, VIBE_SETTINGS_ROTATE, !s->snapshot.settings.auto_rotate);
        break;
    case VUI_VIBE_ACT_ID_TOGGLE_HAPTICS:
        if (s->snapshot.device.haptics_available) number_setting(s, VIBE_SETTINGS_HAPTICS, !s->snapshot.settings.haptics);
        break;
    case VUI_VIBE_ACT_ID_INSTALL:
        if (s->page == VIBE_CONFIRM) { s->run_development_pending = false; open_confirm(s); }
        break;
    case VUI_VIBE_ACT_ID_RESULT_OPEN:
        if (s->page == VIBE_RESULT && s->result_can_open && !s->result_failed && !s->result_cancelled) { s->run_development_pending = false; open_confirm(s); }
        break;
    case VUI_VIBE_ACT_ID_RESULT_DONE:
        if (s->page == VIBE_RESULT) {
            if (s->result_edit_code) {
                s->result_edit_code = false;
                s->code_submitted = s->code_incomplete = false;
                vibe_show(s, VIBE_BRIDGE);
            } else
                vibe_show(s, s->result_cancelled ? VIBE_HOME : VIBE_NAND);
        }
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
        s->run_development_pending = false;
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
    case VUI_VIBE_ACT_ID_FORMAT_NAND:
        if (s->page == VIBE_NAND) confirm_nand_format(s);
        break;
    case VUI_VIBE_ACT_ID_FORMAT_NAND_ACCEPT: {
        if (s->page != VIBE_FORMAT_NAND_CONFIRM) break;
        s->services.snapshot(s->services.ctx, &s->snapshot);
        if (!s->snapshot.nand_needs_format) { vibe_show(s, s->format_return); break; }
        if (s->snapshot.idea_busy || s->snapshot.idea_ready || s->snapshot.updating ||
            s->snapshot.settings.busy) break;
        memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
        int err = vibe_command(s, VIBE_FORMAT_NAND, NULL, NULL);
        if (err) vibe_error(s, "Format failed", err);
        else { vibe_show(s, VIBE_UPDATE); vibe_ui_poll(s); }
        break;
    }
    case VUI_VIBE_ACT_ID_UPDATE_ALL: {
        /* A scan owns the Ideas worker too; keep its progress visible instead
         * of turning an early tap into an operation-failed result. */
        if (s->snapshot.idea_busy && !s->snapshot.batch) break;
        if (s->page != VIBE_NAND || s->snapshot.nand_error || !commit_delete(s)) break;
        if (s->snapshot.batch) {
            int err = vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL);
            if (err) vibe_error(s, "Operation failed", err);
            else s->batch_cancel_pending = true;
            break;
        }
        vibe_show(s, VIBE_UPDATE_CONFIRM);
        break;
    }
    case VUI_VIBE_ACT_ID_UPDATE_ALL_ACCEPT: {
        if (s->page != VIBE_UPDATE_CONFIRM) break;
        s->services.snapshot(s->services.ctx, &s->snapshot);
        if (s->snapshot.idea_busy || s->snapshot.batch || s->snapshot.nand_error) break;
        memset(&s->selected_bundle, 0, sizeof(s->selected_bundle));
        int err = vibe_command(s, VIBE_UPDATE_ALL, NULL, NULL);
        if (err) vibe_error(s, "Operation failed", err);
        else vibe_show(s, VIBE_NAND);
        break;
    }
    case VUI_VIBE_ACT_ID_RETRY_STORAGE:
        if (s->page == VIBE_NAND && !s->snapshot.idea_busy) {
            int err = vibe_command(s, VIBE_SCAN_NAND, NULL, NULL);
            if (err) vibe_error(s, "Operation failed", err);
        }
        break;
    case VUI_VIBE_ACT_ID_CANCEL_DOWNLOAD:
        if (s->snapshot.cancellable && !vibe_command(s, VIBE_CANCEL_IDEA, NULL, NULL)) {
            s->snapshot.cancellable = false;
            TEXT(s, UPDATE_TITLE, "Cancelling...");
            TEXT(s, UPDATE_DETAIL, "Please wait");
            (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_CANCEL_DOWNLOAD_VISIBLE, false);
        }
        break;
    case VUI_VIBE_ACT_ID_SHARED_PREV:
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT, vui_scroll_offset(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT) - 320);
        break;
    case VUI_VIBE_ACT_ID_SHARED_NEXT:
        (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT, vui_scroll_offset(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT) + 320);
        break;
    case VUI_VIBE_ACT_ID_SHARED_ADD:
        memset(s->shared_key, 0, sizeof(s->shared_key)); memset(s->shared_value, 0, sizeof(s->shared_value));
        s->shared_system_entry = false; s->shared_rename_mode = false;
        s->shared_edit_value = s->shared_reveal = s->shared_detail_reveal = s->shared_wait = false; vibe_show(s, VIBE_SHARED_EDITOR); project_shared(s); break;
    case VUI_VIBE_ACT_ID_SHARED_ROW: {
        size_t user_cnt = shared_user_count(&s->snapshot.settings);
        size_t i = e->arg;
        if (s->page != VIBE_SHARED || i >= user_cnt + 7) break;
        if (i < user_cnt) {
            const vibe_shared_entry_t *entry = shared_user_entry(&s->snapshot.settings, i);
            if (!entry) break;
            if (s->undo_until && s->undo_shared && !strcmp(entry->key, s->undo_key)) {
                s->undo_until = 0; break;
            }
            snprintf(s->shared_key, sizeof(s->shared_key), "%s", entry->key);
            snprintf(s->shared_value, sizeof(s->shared_value), "%s", entry->value);
            s->shared_system_entry = false;
        } else {
            size_t sys = i - user_cnt;
            snprintf(s->shared_key, sizeof(s->shared_key), "%s", s_shared_sys_keys[sys]);
            s->shared_value[0] = 0;
            s->shared_system_entry = true;
        }
        s->shared_rename_mode = false;
        s->shared_edit_value = true; s->shared_reveal = s->shared_detail_reveal = s->shared_wait = false;
        vibe_show(s, VIBE_SHARED_DETAIL); break;
    }
    case VUI_VIBE_ACT_ID_SHARED_REVEAL: s->shared_reveal = !s->shared_reveal; project_shared(s); break;
    case VUI_VIBE_ACT_ID_SHARED_SUBMIT: if (s->page == VIBE_SHARED_EDITOR) shared_submit(s); break;
    case VUI_VIBE_ACT_ID_SHARED_DELETE:
        if (s->page == VIBE_SHARED_DETAIL && s->shared_system_entry) {
            open_shared_system_settings(s);
            break;
        }
        if ((s->page == VIBE_SHARED_EDITOR || s->page == VIBE_SHARED_DETAIL) && s->shared_key[0] && commit_delete(s)) {
            snprintf(s->undo_key, sizeof(s->undo_key), "%s", s->shared_rename_mode ? s->shared_orig_key : s->shared_key);
            s->delete_code[0] = 0;
            s->return_page = s->page;
            TEXT(s, DELETE_CONFIRM_TITLE, "Shared information");
            vibe_show(s, VIBE_DELETE_CONFIRM);
        }
        break;
    case VUI_VIBE_ACT_ID_SHARED_KEYBOARD_KEY: {
        if (s->page != VIBE_SHARED_EDITOR || s->shared_wait) break;
        if (e->arg == 13) { shared_submit(s); break; }
        char *value = s->shared_edit_value ? s->shared_value : s->shared_key;
        size_t n = strlen(value), cap = s->shared_edit_value ? sizeof(s->shared_value) : sizeof(s->shared_key);
        if (s->shared_edit_value && !strcmp(s->shared_key, "wifi_ssid")) cap = FACTORY_NETWORK_SSID_BYTES;
        if (s->shared_edit_value && !strcmp(s->shared_key, "wifi_password")) cap = FACTORY_NETWORK_PASSWORD_BYTES;
        if (e->arg == 8 && n) value[--n] = 0;
        else if (e->arg >= 32 && e->arg <= 126 && n + 1 < cap) { value[n] = !s->shared_edit_value && e->arg == 32 ? '_' : (char)e->arg; value[n + 1] = 0; }
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
/* Provider titles preserve arbitrary downloaded glyphs without adding them to
 * the built-in UI font. Retry transient cache misses only on the visible page. */
static void render_grid_titles(vibe_ui_t *s, unsigned page)
{
    static const uint16_t names[] = {VUI_VIBE_BIND_CELL_NAME_0, VUI_VIBE_BIND_CELL_NAME_1, VUI_VIBE_BIND_CELL_NAME_2, VUI_VIBE_BIND_CELL_NAME_3, VUI_VIBE_BIND_CELL_NAME_4, VUI_VIBE_BIND_CELL_NAME_5};
    static const uint16_t titles[] = {VUI_VIBE_BIND_CELL_TITLE_0, VUI_VIBE_BIND_CELL_TITLE_1, VUI_VIBE_BIND_CELL_TITLE_2, VUI_VIBE_BIND_CELL_TITLE_3, VUI_VIBE_BIND_CELL_TITLE_4, VUI_VIBE_BIND_CELL_TITLE_5};
    s->grid_titles_pending = false;
    size_t count = vibe_bundle_count(s);
    for (unsigned cell = 0; cell < 6; ++cell) {
        size_t i = vibe_bundle_index(s, page * 6 + cell);
        const vibe_bundle_t *idea = i < s->snapshot.bundle_count ? &s->snapshot.bundles[i] : NULL;
        const char *name = idea ? idea->release :
            count && page * 6 + cell == count ? vibe_translate(s, "Add Idea") : "";
        bool ascii = true;
        for (const unsigned char *c = (const unsigned char *)name; idea && *c; ++c)
            if (*c < 32 || *c > 126) { ascii = false; break; }
        bool has_title = (s->grid_title_ready_mask & (1u << cell)) != 0;
        if (idea && !ascii && s->services.title && !has_title) {
            size_t size = 0;
            void *image = s->services.title(s->services.ctx, idea->code, &size);
            if (image) {
                has_title = vui_set_image_owned(s->ui, titles[cell], image, size) == VUI_OK;
                if (!has_title) free(image);
                else s->grid_title_ready_mask |= 1u << cell;
            }
            s->grid_titles_pending |= !has_title;
        }
        char fallback[24] = "";
        if (idea && !*name) snprintf(fallback, sizeof(fallback), "Idea %s", idea->code);
        /* A pending non-ASCII name has a neutral placeholder, never its ID.
         * Do not send unsupported UTF-8 to the fixed firmware font. */
        (void)vui_set_text(s->ui, names[cell], has_title ? "" :
            !ascii ? "..." : *name ? name : fallback);
        (void)vui_set_visible(s->ui, titles[cell], has_title);
        if (!has_title) (void)vui_set_image_owned(s->ui, titles[cell], NULL, 0);
    }
}
static void render_grid(vibe_ui_t *s, unsigned page)
{
    s->grid_title_ready_mask = 0;
    static const uint16_t updates[] = {VUI_VIBE_BIND_CELL_UPDATE_0_VISIBLE, VUI_VIBE_BIND_CELL_UPDATE_1_VISIBLE,
        VUI_VIBE_BIND_CELL_UPDATE_2_VISIBLE, VUI_VIBE_BIND_CELL_UPDATE_3_VISIBLE, VUI_VIBE_BIND_CELL_UPDATE_4_VISIBLE, VUI_VIBE_BIND_CELL_UPDATE_5_VISIBLE};
    static const uint16_t icons[] = {VUI_VIBE_BIND_CELL_ICON_0, VUI_VIBE_BIND_CELL_ICON_1, VUI_VIBE_BIND_CELL_ICON_2, VUI_VIBE_BIND_CELL_ICON_3, VUI_VIBE_BIND_CELL_ICON_4, VUI_VIBE_BIND_CELL_ICON_5};
    static const uint16_t icon_objects[] = {VUI_VIBE_BIND_CELL_ICON_0_VISIBLE, VUI_VIBE_BIND_CELL_ICON_1_VISIBLE, VUI_VIBE_BIND_CELL_ICON_2_VISIBLE, VUI_VIBE_BIND_CELL_ICON_3_VISIBLE, VUI_VIBE_BIND_CELL_ICON_4_VISIBLE, VUI_VIBE_BIND_CELL_ICON_5_VISIBLE};
    static const uint16_t defaults[] = {VUI_VIBE_BIND_CELL_DEFAULT_0_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_1_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_2_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_3_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_4_VISIBLE, VUI_VIBE_BIND_CELL_DEFAULT_5_VISIBLE};
    static const uint16_t current[] = {VUI_VIBE_BIND_CELL_CURRENT_0_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_1_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_2_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_3_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_4_VISIBLE, VUI_VIBE_BIND_CELL_CURRENT_5_VISIBLE};
    static const uint16_t cards[] = {VUI_VIBE_BIND_BUTTON_CELL_0_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_1_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_2_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_3_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_4_VISIBLE, VUI_VIBE_BIND_BUTTON_CELL_5_VISIBLE};
    static const uint16_t add[] = {VUI_VIBE_BIND_CELL_ADD_0_VISIBLE, VUI_VIBE_BIND_CELL_ADD_1_VISIBLE, VUI_VIBE_BIND_CELL_ADD_2_VISIBLE,
        VUI_VIBE_BIND_CELL_ADD_3_VISIBLE, VUI_VIBE_BIND_CELL_ADD_4_VISIBLE, VUI_VIBE_BIND_CELL_ADD_5_VISIBLE};
    size_t visible = vibe_bundle_count(s);
    if (page > visible / 6) page = (unsigned)visible / 6;
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_EMPTY_VISIBLE, !visible);
    for (size_t cell = 0; cell < 6; ++cell) {
        size_t i = vibe_bundle_index(s, page * 6 + cell);
        bool present = i < s->snapshot.bundle_count;
        const vibe_bundle_t *idea = present ? &s->snapshot.bundles[i] : NULL;
        bool add_cell = visible && page * 6 + cell == visible;
        size_t size = 0;
        void *data = idea && s->services.icon ? s->services.icon(s->services.ctx, idea->code, &size) : NULL;
        bool has_icon = data != NULL;
        if (data && vui_set_image_owned(s->ui, icons[cell], data, size) != VUI_OK) { free(data); has_icon = false; }
        (void)vui_set_visible(s->ui, icon_objects[cell], has_icon);
        (void)vui_set_visible(s->ui, defaults[cell], present && !has_icon);
        (void)vui_set_visible(s->ui, current[cell], idea && idea->boot);
        (void)vui_set_visible(s->ui, updates[cell], idea && idea->update_available);
        (void)vui_set_visible(s->ui, cards[cell], visible ? present || add_cell : cell == 1);
        (void)vui_set_visible(s->ui, add[cell], add_cell);
    }
    render_grid_titles(s, page);
    if (visible && s->nav_badge_until && vui_now_ms(s->ui) < s->nav_badge_until)
        FORMAT(s, GRID_TITLE_TOP, "Ideas %u/%u", page + 1, (unsigned)visible / 6 + 1);
    else TEXT(s, GRID_TITLE_TOP, "");
    s->grid_generation = s->snapshot.nand_generation + 1;
    s->grid_rendered_page = page;
}
enum { PAGER_WIDTH = 480, PAGER_TOP = 56, PAGER_BOTTOM = 448,
       PAGER_START = 24, PAGER_SLOP = 12, PAGER_FLICK_DISTANCE = 32, PAGER_FLICK_SPEED = 450 };
static int pager_clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}
static unsigned pager_origin(const vibe_pager_t *p)
{
    return p->origin == VIBE_HOME ? 0 : p->grid + 1;
}
static void pager_project(vibe_ui_t *s, unsigned index)
{
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_VISIBLE, !index);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_VISIBLE, index != 0);
    if (index && (s->grid_generation != s->snapshot.nand_generation + 1 ||
                  s->grid_rendered_page != index - 1)) render_grid(s, index - 1);
}
static void update_nav_badge(vibe_ui_t *s)
{
    bool show = s->nav_badge_until && vui_now_ms(s->ui) < s->nav_badge_until && !s->pager.active;
    if (!show) {
        if (s->nav_badge_until && !s->pager.active) s->nav_badge_until = 0;
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_HOME_NAV_BADGE, "");
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_GRID_TITLE_TOP, "");
        return;
    }
    if (s->page == VIBE_HOME) {
        TEXT(s, HOME_NAV_BADGE, "Home");
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_GRID_TITLE_TOP, "");
    } else if (s->page == VIBE_GRID) {
        (void)vui_set_text(s->ui, VUI_VIBE_BIND_HOME_NAV_BADGE, "");
        size_t visible = vibe_bundle_count(s);
        FORMAT(s, GRID_TITLE_TOP, "Ideas %u/%u", s->grid_page + 1, (unsigned)visible / 6 + 1);
    }
}
static void pager_finish(vibe_ui_t *s, bool commit)
{
    vibe_pager_t *p = &s->pager;
    unsigned origin = pager_origin(p);
    unsigned index = commit && p->target >= 0 ? (unsigned)p->target : origin;
    unsigned original_grid = p->grid;
    if (p->timer) (void)vui_timer_delete(s->ui, p->timer);
    memset(p, 0, sizeof(*p));
    vui_transition_end(s->ui);
    s->grid_page = index ? index - 1 : original_grid;
    pager_project(s, index);
    const vibe_page_t page = index ? VIBE_GRID : VIBE_HOME;
    if (page != s->page) vibe_show(s, page);
    s->pager_state = 0;
    render_pager(s);
    if (commit && index != origin) {
        s->nav_badge_until = vui_now_ms(s->ui) + 2000;
        update_nav_badge(s);
    }
}
void vibe_product_cancel_pager(vibe_ui_t *s)
{
    if (!s->pager.active && !s->pager.candidate) return;
    if (s->pager.active) pager_finish(s, false);
    else memset(&s->pager, 0, sizeof(s->pager));
    s->pager.blocked = s->pointer_down;
}
static void pager_move(vibe_ui_t *s, int x)
{
    vibe_pager_t *p = &s->pager;
    int shift = p->base_shift + x - p->anchor_x;
    shift = shift > p->slop ? shift - p->slop : shift < -p->slop ? shift + p->slop : 0;
    p->shift = pager_clamp(shift, -PAGER_WIDTH, PAGER_WIDTH);
    if (p->shift) {
        int target = (int)pager_origin(p) + (p->shift < 0 ? 1 : -1);
        if (target < 0 || target >= (int)(p->count / 6 + 2)) target = -1;
        if (target != p->target) {
            p->target = target;
            if (p->live && target >= 0) pager_project(s, (unsigned)target);
        }
    }
    p->offset = p->target >= 0 ? p->shift : pager_clamp(p->shift / 4, -60, 60);
    if (p->live) vui_transition_update(s->ui, p->offset, p->target >= 0);
}
static void pager_tick(vui_handle_t ui, void *ctx)
{
    vibe_ui_t *s = ctx;
    vibe_pager_t *p = &s->pager;
    if (!p->active) return;
    if (p->count != vibe_bundle_count(s)) {
        vibe_product_cancel_pager(s);
        return;
    }
    if (p->generation != s->snapshot.nand_generation) {
        /* Update badges and title loads share this generation with the library.
         * Only replacing/reordering actual targets invalidates a page drag. */
        for (unsigned i = 0; i < p->count; ++i) {
            size_t index = vibe_bundle_index(s, i);
            if (strncmp(p->codes[i], s->snapshot.bundles[index].code, sizeof(p->codes[i]))) {
                vibe_product_cancel_pager(s);
                return;
            }
        }
        p->generation = s->snapshot.nand_generation;
    }
    if (!p->settling) return;
    uint64_t elapsed = vui_now_ms(ui) - p->settle_start;
    if (elapsed >= p->settle_duration) {
        pager_finish(s, p->settle_to != 0);
        return;
    }
    int64_t t = (int64_t)elapsed * 65536 / p->settle_duration;
    int64_t t2 = t * t >> 16, t3 = t2 * t >> 16;
    int64_t value = (2 * t3 - 3 * t2 + 65536) * p->settle_from +
                    (t3 - 2 * t2 + t) * p->settle_tangent +
                    (-2 * t3 + 3 * t2) * p->settle_to;
    int position = (int)(value / 65536);
    p->offset = p->settle_from < p->settle_to
        ? pager_clamp(position, p->settle_from, p->settle_to)
        : pager_clamp(position, p->settle_to, p->settle_from);
    if (p->live) vui_transition_update(ui, p->offset, p->target >= 0);
}
static bool pager_pointer(vibe_ui_t *s, int x, int y, bool pressed)
{
    vibe_pager_t *p = &s->pager;
    if (p->blocked) {
        if (!pressed) p->blocked = false;
        s->gesture_consumed = true;
        return true;
    }
    if (pressed && !s->pointer_down) {
        if (p->active && p->settling) {
            if (y < PAGER_TOP || y >= PAGER_BOTTOM) {
                bool commit = p->settle_to != 0;
                pager_finish(s, commit);
                p->blocked = true;
            } else {
                if (p->timer) (void)vui_timer_delete(s->ui, p->timer);
                p->timer = NULL;
                p->settling = false;
                p->anchor_x = x;
                p->base_shift = p->target >= 0 ? p->offset : p->offset * 4;
                p->slop = 0;
            }
            s->home_capture = false;
            s->gesture_consumed = true;
            return true;
        }
        memset(p, 0, sizeof(*p));
        p->candidate = (s->page == VIBE_HOME || s->page == VIBE_GRID) &&
            s->pointer_page_ready && vui_now_ms(s->ui) >= s->suppress_until &&
            y >= PAGER_TOP && y < PAGER_BOTTOM;
        return false;
    }
    if (!p->candidate && !p->active) return false;
    if (!p->active) {
        int dx = x - s->pointer_x, dy = y - s->pointer_y;
        if (abs(dy) >= PAGER_START && abs(dy) >= abs(dx)) p->vertical = true;
        if (p->vertical || abs(dx) < PAGER_START || abs(dx) * 4 < abs(dy) * 5) {
            if (!pressed) p->candidate = false;
            return false;
        }
        p->origin = s->page;
        p->grid = s->grid_page;
        p->count = (unsigned)vibe_bundle_count(s);
        p->generation = s->snapshot.nand_generation;
        for (unsigned i = 0; i < p->count; ++i)
            memcpy(p->codes[i], s->snapshot.bundles[vibe_bundle_index(s, i)].code, sizeof(p->codes[i]));
        p->anchor_x = s->pointer_x;
        p->slop = PAGER_SLOP;
        p->target = -2;
        if (s->home_capture) {
            vibe_home_motion_input(&s->home_motion, 0, 0, false, true);
            s->home_capture = false;
        }
        p->live = vui_transition_begin(s->ui, (vui_area_t){0, PAGER_TOP, PAGER_WIDTH,
                                                       PAGER_BOTTOM - PAGER_TOP}) == VUI_OK;
        p->active = true;
    }
    s->gesture_consumed = true;
    pager_move(s, x);
    if (!pressed) {
        int velocity = vui_pointer_velocity_x(s->ui);
        int direction = p->shift < 0 ? -1 : 1;
        int distance = abs(p->shift);
        bool commit = p->target >= 0 && (distance >= PAGER_WIDTH / 4 ||
            (distance + p->slop >= PAGER_FLICK_DISTANCE && velocity * direction >= PAGER_FLICK_SPEED));
        if (distance < PAGER_WIDTH * 17 / 20 && velocity * direction < 0) commit = false;
        p->settle_from = p->offset;
        p->settle_to = commit ? direction * PAGER_WIDTH : 0;
        int delta = p->settle_to - p->offset;
        if (!p->live || !delta) { pager_finish(s, commit); return true; }
        int reduction = velocity * (delta < 0 ? -1 : 1) * 60 / PAGER_WIDTH;
        reduction = pager_clamp(reduction, 0, 120);
        p->settle_duration = (unsigned)pager_clamp(120 + abs(delta) * 80 / PAGER_WIDTH - reduction, 80, 220);
        int tangent = velocity * (int)p->settle_duration / 1000;
        p->settle_tangent = delta > 0 ? pager_clamp(tangent, delta * 2, delta * 3)
                                     : pager_clamp(tangent, delta * 3, delta * 2);
        p->settle_start = vui_now_ms(s->ui);
        p->settling = true;
        p->timer = vui_timer_create(s->ui, 16, pager_tick, s);
        if (!p->timer) pager_finish(s, commit);
    }
    return true;
}
void vibe_product_poll(vibe_ui_t *s)
{
    const vibe_snapshot_t *v = &s->snapshot;
    if (s->page == VIBE_OPEN_CONFIRM) project_open_confirm(s);
    TEXT(s, BATCH_BUTTON, v->batch ? "Cancel" : "Update all");
    (void)vui_set_text(s->ui, VUI_VIBE_BIND_TOGGLE_OPEN_CONFIRM_LABEL, "");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_OPEN_CONFIRM_CHECKED_VISIBLE, !v->settings.open_confirm);
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
            s->shared_wait = false;
            if (s->shared_rename_mode) {
                s->shared_rename_mode = false;
                s->shared_edit_value = true;
                vibe_show(s, VIBE_SHARED_DETAIL);
            } else {
                memset(s->shared_value, 0, sizeof(s->shared_value));
                vibe_show(s, VIBE_SHARED);
            }
        }
    }
    if (s->first_run && s->page == VIBE_WIFI && v->network == VIBE_NET_CONNECTED) {
        s->first_run = false; vibe_show(s, VIBE_HOME);
    }
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_CANCEL_DOWNLOAD_VISIBLE, v->cancellable);
    bool result_open = s->result_can_open && !s->result_failed && !s->result_cancelled && s->selected_bundle.code[0];
    TEXT(s, RESULT_DONE_LABEL, s->result_edit_code ? "Edit code" : "Done");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_RESULT_OPEN_VISIBLE, result_open);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_RESULT_LIBRARY_VISIBLE, result_open);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_RESULT_DONE_VISIBLE, !result_open);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_UNDO_DELETE_VISIBLE, s->undo_until != 0);
    if (s->undo_until) FORMAT(s, UNDO_MESSAGE, "Deleted · %u s to undo",
        (unsigned)((s->undo_until > vui_now_ms(s->ui) ? s->undo_until - vui_now_ms(s->ui) : 0) + 999) / 1000);
    else TEXT(s, UNDO_MESSAGE, "");
    const bool storage_error = v->nand_error != 0 && !v->nand_scanning;
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_LIBRARY_EMPTY_VISIBLE,
        s->page == VIBE_NAND && !v->nand_needs_format && !v->nand_scanning && (storage_error || !vibe_bundle_count(s)));
    TEXT(s, LIBRARY_EMPTY_TITLE, storage_error ? "Unavailable" : "No Ideas yet");
    TEXT(s, LIBRARY_EMPTY_NOTE, storage_error ? "Your saved files are kept." : "Tap + to download your first Idea.");
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_RETRY_STORAGE_VISIBLE,
        storage_error && !v->nand_needs_format && !v->idea_busy);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_MANAGE_UPDATE_ACTION_VISIBLE,
        !v->nand_error && vibe_bundle_count(s) != 0);
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_SHARED_EMPTY_VISIBLE, false);
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
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_FORMAT_NAND_VISIBLE,
                          s->page == VIBE_NAND && v->nand_needs_format && !v->idea_busy);
    if (!v->nand_needs_format && !v->idea_busy && !v->updating)
        s->nand_prompt_dismissed = false;
    if (v->nand_needs_format && !s->nand_prompt_dismissed && !s->first_run &&
        (s->page == VIBE_HOME || s->page == VIBE_NAND || s->page == VIBE_GRID ||
         s->page == VIBE_BRIDGE || s->page == VIBE_RESULT))
        confirm_nand_format(s);
    const bool discover_library = (s->page == VIBE_NAND || s->page == VIBE_GRID) &&
                                  !v->nand_generation && !v->idea_busy && !v->nand_error;
    if (discover_library)
        (void)vibe_command(s, VIBE_SCAN_NAND, NULL, NULL);
    if (!discover_library && (s->page == VIBE_NAND || s->page == VIBE_ABOUT) &&
        !v->storage_total && !v->idea_busy && !v->nand_error)
        (void)vibe_command(s, VIBE_STORAGE_INFO, NULL, NULL);
    if (s->page == VIBE_NAND) {
        if (v->nand_scanning) TEXT(s, NAND_SCAN, "Checking apps and storage...");
        else if (v->nand_error) FORMAT(s, NAND_SCAN, "NAND unavailable: 0x%x", (unsigned)v->nand_error);
        else if (!v->storage_total) TEXT(s, NAND_SCAN, "Checking apps and storage...");
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
        static const uint16_t focuses[] = {VUI_VIBE_BIND_CODE_FOCUS_0_VISIBLE, VUI_VIBE_BIND_CODE_FOCUS_1_VISIBLE, VUI_VIBE_BIND_CODE_FOCUS_2_VISIBLE,
                                           VUI_VIBE_BIND_CODE_FOCUS_3_VISIBLE, VUI_VIBE_BIND_CODE_FOCUS_4_VISIBLE, VUI_VIBE_BIND_CODE_FOCUS_5_VISIBLE};
        size_t len = strlen(s->idea_code);
        for (unsigned i = 0; i < 6; ++i) {
            char value[2] = {i < len ? s->idea_code[i] : ' ', 0};
            (void)vui_set_text(s->ui, digits[i], value);
            (void)vui_set_visible(s->ui, focuses[i], i == (len < 6 ? len : 5));
        }
    }
    if (s->page == VIBE_GUIDE) {
        static const char *const g_titles[4] = {
            "Vibe Coding Guide", "Getting Started", "Idea IDs & Sharing", "Troubleshooting"
        };
        static const char *const g_descs[4] = {
            "Build your first Idea with AI.",
            "Set up and run your first Idea.",
            "Find Idea IDs and share your own.",
            "Help with Wi-Fi, storage and Ideas."
        };
        static const char *const g_urls[4] = {
            "mosaico.espressif.com/guide/vibe-coding",
            "mosaico.espressif.com/vibe/start",
            "mosaico.espressif.com/vibe/ids",
            "mosaico.espressif.com/vibe/fix"
        };
        unsigned gt = s->guide_topic < 4 ? s->guide_topic : 0;
        TEXT(s, GUIDE_HEADING, g_titles[gt]);
        TEXT(s, GUIDE_ONE, g_descs[gt]);
        TEXT(s, GUIDE_URL, g_urls[gt]);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GUIDE_QR_DEVELOP_VISIBLE, gt == 0);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GUIDE_QR_START_VISIBLE, gt == 1);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GUIDE_QR_IDS_VISIBLE, gt == 2);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GUIDE_QR_FIX_VISIBLE, gt == 3);
    }
    if (s->page == VIBE_HELP) project_help(s);
    if (s->page == VIBE_RESET) {
        TEXT(s, RESET_VAL_1, v->network == VIBE_NET_CONNECTED && v->ssid[0] ? v->ssid : "Not connected");
        FORMAT(s, RESET_VAL_2, "%u entries", (unsigned)v->settings.shared_count);
    }
    if (s->page == VIBE_HOME || s->page == VIBE_GRID) {
        TEXT(s, HOME_CAPTION, v->network == VIBE_NET_CONNECTED ? "Snap ideas together." : "");
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_WIFI_PILL_VISIBLE, v->network != VIBE_NET_CONNECTED);
        if (v->nand_generation) FORMAT(s, HOME_IDEAS_COUNT, "%u", (unsigned)vibe_bundle_count(s));
        else TEXT(s, HOME_IDEAS_COUNT, "--");
        FORMAT(s, HOME_VERSION, "%s", v->firmware_version);
        FORMAT(s, GRID_VERSION, "%s", v->firmware_version);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_HEADING_EN_VISIBLE, v->settings.language != 2);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HOME_HEADING_ZH_VISIBLE, v->settings.language == 2);
        const bool wifi_on = v->network == VIBE_NET_CONNECTED;
        const bool usb_on = v->device.usb_connected;
        const bool usb_agent = usb_on && v->usb_owner;
        const bool bat_valid = v->device.battery_valid;
        const bool bat_chg = bat_valid && v->device.charging;
        const unsigned pct = v->device.battery_percent;
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_WIFI_VISIBLE, wifi_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_WIFI_OFF_VISIBLE, !wifi_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_USB_VISIBLE, usb_on && !usb_agent);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_USB_AGENT_VISIBLE, usb_agent);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_USB_OFF_VISIBLE, !usb_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_FULL_GREEN_VISIBLE, bat_valid && !bat_chg && pct >= 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_VISIBLE, bat_valid && !bat_chg && pct > 60 && pct < 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_MID_VISIBLE, bat_valid && !bat_chg && pct > 20 && pct <= 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_LOW_VISIBLE, bat_valid && !bat_chg && pct <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_CHARGING_GREEN_VISIBLE, bat_chg && pct >= 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_CHARGING_VISIBLE, bat_chg && pct > 20 && pct < 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_CHARGING_YELLOW_VISIBLE, bat_chg && pct <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_STATUS_BATTERY_UNKNOWN_VISIBLE, !bat_valid);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_WIFI_VISIBLE, wifi_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_WIFI_OFF_VISIBLE, !wifi_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_USB_VISIBLE, usb_on && !usb_agent);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_USB_AGENT_VISIBLE, usb_agent);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_USB_OFF_VISIBLE, !usb_on);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_FULL_GREEN_VISIBLE, bat_valid && !bat_chg && pct >= 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_VISIBLE, bat_valid && !bat_chg && pct > 60 && pct < 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_MID_VISIBLE, bat_valid && !bat_chg && pct > 20 && pct <= 60);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_LOW_VISIBLE, bat_valid && !bat_chg && pct <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_CHARGING_GREEN_VISIBLE, bat_chg && pct >= 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_CHARGING_VISIBLE, bat_chg && pct > 20 && pct < 95);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_CHARGING_YELLOW_VISIBLE, bat_chg && pct <= 20);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_GRID_STATUS_BATTERY_UNKNOWN_VISIBLE, !bat_valid);
        char battery[24] = "";
        if (v->device.battery_valid) snprintf(battery, sizeof(battery), "%u%%%s", v->device.battery_percent, v->device.charging ? "+" : "");
        FORMAT(s, HOME_STATUS, "%s %s %s", v->network == VIBE_NET_CONNECTED ? "Wi-Fi" : "", v->device.usb_connected ? "USB" : "", battery);
    }
    if (s->page == VIBE_GRID && !s->pager.active) {
        if (s->grid_page > vibe_bundle_count(s) / 6) s->grid_page = (unsigned)vibe_bundle_count(s) / 6;
        if (s->grid_generation != v->nand_generation + 1 || s->grid_rendered_page != s->grid_page)
            render_grid(s, s->grid_page);
        else if (s->grid_titles_pending) render_grid_titles(s, s->grid_page);
    } else if (s->page == VIBE_DEVELOP) {
        TEXT(s, DEVELOP_USB, v->device.usb_connected ? "Device connected" : "USB not connected");
        TEXT(s, DEVELOP_AGENT, !v->device.usb_connected ? "Connect USB to develop" : v->usb_owner ? "Agent ready" : "Waiting for Agent");
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_DEVELOP_DOT_READY_VISIBLE, v->device.usb_connected && v->usb_owner);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_DEVELOP_DOT_WAIT_VISIBLE, v->device.usb_connected && !v->usb_owner);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_DEVELOP_DOT_OFF_VISIBLE, !v->device.usb_connected);
        TEXT(s, DEVELOP_RUN_DETAIL, !v->development_available ? "No development build" : v->development_name);
        TEXT(s, DEVELOP_SAVE_DETAIL, !v->development_available ? "No development build" : v->development_saved ? "Already saved" : v->development_name);
    } else if (s->page == VIBE_DISPLAY) {
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_ROTATE_0_0_VISIBLE, !v->device.imu_available || !v->settings.auto_rotate);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_ROTATE_0_1_VISIBLE, v->device.imu_available && v->settings.auto_rotate);
        TEXT(s, DISPLAY_ROTATE_SUB, v->device.imu_available ? "Follows how you hold it" :
            v->device.peripherals_ready ? "Unavailable" : "Starting...");
        TEXT(s, DISPLAY_HAPTICS_SUB, v->device.haptics_available ? (v->settings.haptics ? "Feedback for active controls" : "Off") :
            v->device.peripherals_ready ? "Unavailable" : "Starting...");
        static const uint16_t brightness[] = {VUI_VIBE_BIND_BRIGHTNESS_SELECTED_0_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_1_VISIBLE,
            VUI_VIBE_BIND_BRIGHTNESS_SELECTED_2_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_3_VISIBLE, VUI_VIBE_BIND_BRIGHTNESS_SELECTED_4_VISIBLE};
        static const uint16_t timeout[] = {VUI_VIBE_BIND_TIMEOUT_SELECTED_0_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_1_VISIBLE,
            VUI_VIBE_BIND_TIMEOUT_SELECTED_2_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_3_VISIBLE, VUI_VIBE_BIND_TIMEOUT_SELECTED_4_VISIBLE};
        static const unsigned seconds[] = {60, 180, 300, 600, 0};
        for (unsigned i = 0; i < 5; ++i) {
            (void)vui_set_visible(s->ui, brightness[i], v->settings.brightness == (i + 1) * 20);
            (void)vui_set_visible(s->ui, timeout[i], v->settings.screen_off_seconds == seconds[i]);
        }
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HAPTICS_0_0_VISIBLE, !v->device.haptics_available || !v->settings.haptics);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_HAPTICS_0_1_VISIBLE, v->device.haptics_available && v->settings.haptics);
        TEXT(s, TOGGLE_ROTATE_LABEL, v->settings.auto_rotate ? "Auto rotate: on" : "Auto rotate: off");
        TEXT(s, TOGGLE_HAPTICS_LABEL, v->settings.haptics ? "Haptics: on" : "Haptics: off");
        FORMAT(s, DISPLAY_SUMMARY, "Brightness %u%% · timeout %u s", v->settings.brightness, v->settings.screen_off_seconds);
    } else if (s->page == VIBE_ABOUT) {
        if (s->firmware_check == CHECK_PENDING && !v->release_checking && v->release_generation != s->firmware_check_generation) {
            s->firmware_check = v->release_error ? CHECK_FAILED : CHECK_CURRENT;
            if (!v->release_error && v->release_available) {
                vibe_show(s, VIBE_FIRMWARE_NOTICE);
                return;
            }
        }
        TEXT(s, ABOUT_CHECK, s->firmware_check == CHECK_PENDING ? "Checking..." :
            s->firmware_check == CHECK_CURRENT ? "Up to date" : s->firmware_check == CHECK_FAILED ? "Check failed" :
            s->firmware_check == CHECK_OFFLINE ? "Connect Wi-Fi first" : "Check updates");
        FORMAT(s, ABOUT_VERSION, "%s", v->firmware_version);
        FORMAT(s, ABOUT_MAC, "%s", v->mac);
        if (!v->storage_total || v->nand_error) TEXT(s, ABOUT_STORAGE, "Unknown");
        else FORMAT(s, ABOUT_STORAGE, "%llu / %llu MiB", (unsigned long long)(v->storage_used / 1048576), (unsigned long long)(v->storage_total / 1048576));
    } else if (s->page == VIBE_SHARED) {
        size_t user_cnt = shared_user_count(&v->settings);
        unsigned total_items = (unsigned)user_cnt + 7;
        if (total_items > 23) total_items = 23;
        unsigned content_h = total_items * 80 > 368 ? total_items * 80 : 368;
        (void)vui_set_value(s->ui, VUI_VIBE_BIND_SHARED_VIEWPORT_HEIGHT, content_h);
        if (user_cnt < s->last_shared_user_count) {
            int max_off = (int)content_h - 368;
            int cur_off = vui_scroll_offset(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT);
            if (cur_off > max_off) (void)vui_scroll_to(s->ui, VUI_VIBE_OBJ_KEY_SHARED_VIEWPORT, max_off);
        }
        s->last_shared_user_count = (unsigned)user_cnt;
        const uint16_t rows[] = {
            VUI_VIBE_BIND_SHARED_ROW_0_LABEL, VUI_VIBE_BIND_SHARED_ROW_1_LABEL, VUI_VIBE_BIND_SHARED_ROW_2_LABEL, VUI_VIBE_BIND_SHARED_ROW_3_LABEL
        };
        const uint16_t keys[23] = {
            VUI_VIBE_BIND_SHARED_KEY_0, VUI_VIBE_BIND_SHARED_KEY_1, VUI_VIBE_BIND_SHARED_KEY_2, VUI_VIBE_BIND_SHARED_KEY_3,
            VUI_VIBE_BIND_SHARED_KEY_4, VUI_VIBE_BIND_SHARED_KEY_5, VUI_VIBE_BIND_SHARED_KEY_6, VUI_VIBE_BIND_SHARED_KEY_7,
            VUI_VIBE_BIND_SHARED_KEY_8, VUI_VIBE_BIND_SHARED_KEY_9, VUI_VIBE_BIND_SHARED_KEY_10, VUI_VIBE_BIND_SHARED_KEY_11,
            VUI_VIBE_BIND_SHARED_KEY_12, VUI_VIBE_BIND_SHARED_KEY_13, VUI_VIBE_BIND_SHARED_KEY_14, VUI_VIBE_BIND_SHARED_KEY_15,
            VUI_VIBE_BIND_SHARED_KEY_16, VUI_VIBE_BIND_SHARED_KEY_17, VUI_VIBE_BIND_SHARED_KEY_18, VUI_VIBE_BIND_SHARED_KEY_19,
            VUI_VIBE_BIND_SHARED_KEY_20, VUI_VIBE_BIND_SHARED_KEY_21, VUI_VIBE_BIND_SHARED_KEY_22
        };
        const uint16_t values[23] = {
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_0, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_1, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_2, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_3,
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_4, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_5, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_6, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_7,
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_8, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_9, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_10, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_11,
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_12, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_13, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_14, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_15,
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_16, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_17, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_18, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_19,
            VUI_VIBE_BIND_SHARED_ENTRY_VALUE_20, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_21, VUI_VIBE_BIND_SHARED_ENTRY_VALUE_22
        };
        const uint16_t buttons[23] = {
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_0, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_1, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_2, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_3,
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_4, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_5, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_6, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_7,
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_8, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_9, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_10, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_11,
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_12, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_13, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_14, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_15,
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_16, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_17, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_18, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_19,
            VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_20, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_21, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_ROW_22
        };
        const uint16_t locks[23] = {
            VUI_VIBE_BIND_SHARED_LOCK_0_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_1_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_2_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_3_VISIBLE,
            VUI_VIBE_BIND_SHARED_LOCK_4_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_5_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_6_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_7_VISIBLE,
            VUI_VIBE_BIND_SHARED_LOCK_8_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_9_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_10_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_11_VISIBLE,
            VUI_VIBE_BIND_SHARED_LOCK_12_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_13_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_14_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_15_VISIBLE,
            VUI_VIBE_BIND_SHARED_LOCK_16_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_17_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_18_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_19_VISIBLE,
            VUI_VIBE_BIND_SHARED_LOCK_20_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_21_VISIBLE, VUI_VIBE_BIND_SHARED_LOCK_22_VISIBLE
        };
        const char *stored_ssid = shared_stored_value(&v->settings, "wifi_ssid");
        const char *active_ssid = v->ssid[0] ? v->ssid : (stored_ssid && stored_ssid[0] ? stored_ssid : "-");
        for (size_t r = 0; r < 4; ++r) (void)vui_set_text(s->ui, rows[r], "");
        for (size_t i = 0; i < 23; ++i) {
            (void)vui_set_visible(s->ui, buttons[i], i < total_items);
            (void)vui_set_visible(s->ui, locks[i], i >= user_cnt && i < total_items);
            if (i < user_cnt) {
                const vibe_shared_entry_t *entry = shared_user_entry(&v->settings, i);
                if (!entry) continue;
                (void)vui_set_text(s->ui, keys[i], entry->key);
                if (s->undo_until && s->undo_shared && !strcmp(s->undo_key, entry->key))
                    format(s, values[i], "Undo · %u s",
                        (unsigned)((s->undo_until > vui_now_ms(s->ui) ? s->undo_until - vui_now_ms(s->ui) : 0) + 999) / 1000);
                else (void)vui_set_text(s->ui, values[i], secret(entry->key) ? "********" : entry->value);
            } else if (i < total_items) {
                size_t sys = i - user_cnt;
                char sys_val[64] = "";
                if (sys == 0) snprintf(sys_val, sizeof(sys_val), "%u", v->settings.brightness);
                else if (sys == 1) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.haptics ? "1" : "0");
                else if (sys == 2) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.language == 2 ? "zh-CN" : "en");
                else if (sys == 3) snprintf(sys_val, sizeof(sys_val), "%s", active_ssid);
                else if (sys == 4) snprintf(sys_val, sizeof(sys_val), "********");
                else if (sys == 5) snprintf(sys_val, sizeof(sys_val), "%u", v->settings.screen_off_seconds);
                else if (sys == 6) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.auto_rotate ? "1" : "0");
                (void)vui_set_text(s->ui, keys[i], s_shared_sys_keys[sys]);
                (void)vui_set_text(s->ui, values[i], sys_val);
            }
        }
    } else if (s->page == VIBE_SHARED_DETAIL) {
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_SHARED_DETAIL_SYS_VISIBLE, s->shared_system_entry);
        if (s->shared_system_entry) {
            const char *stored_ssid = shared_stored_value(&v->settings, "wifi_ssid");
            const char *stored_pwd = shared_stored_value(&v->settings, "wifi_password");
            const char *active_ssid = v->ssid[0] ? v->ssid : (stored_ssid && stored_ssid[0] ? stored_ssid : "-");
            char sys_val[64] = "";
            if (!strcmp(s->shared_key, "brightness")) snprintf(sys_val, sizeof(sys_val), "%u", v->settings.brightness);
            else if (!strcmp(s->shared_key, "haptics")) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.haptics ? "1" : "0");
            else if (!strcmp(s->shared_key, "system_language")) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.language == 2 ? "zh-CN" : "en");
            else if (!strcmp(s->shared_key, "wifi_ssid")) snprintf(sys_val, sizeof(sys_val), "%s", active_ssid);
            else if (!strcmp(s->shared_key, "wifi_password"))
                snprintf(sys_val, sizeof(sys_val), "%s", s->shared_detail_reveal ? (stored_pwd && stored_pwd[0] ? stored_pwd : "(saved in NVS)") : "********");
            else if (!strcmp(s->shared_key, "screen_off")) snprintf(sys_val, sizeof(sys_val), "%u", v->settings.screen_off_seconds);
            else if (!strcmp(s->shared_key, "auto_rotate")) snprintf(sys_val, sizeof(sys_val), "%s", v->settings.auto_rotate ? "1" : "0");
            (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_DETAIL_REVEAL_VISIBLE, !strcmp(s->shared_key, "wifi_password"));
            TEXT(s, SHARED_DETAIL_KEY, s->shared_key);
            (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_DETAIL_VALUE, sys_val);
            TEXT(s, SHARED_DETAIL_NOTE, "Read-only system setting · tap to open Settings");
            (void)vui_set_text(s->ui, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_DETAIL_DELETE, vibe_translate(s, "Open Settings"));
        } else {
            /* Reproject after wake or an external settings refresh; editor drafts
             * remain local until the existing save operation completes. */
            for (size_t i = 0; i < v->settings.shared_count; ++i)
                if (!strcmp(v->settings.shared[i].key, s->shared_key))
                    snprintf(s->shared_value, sizeof(s->shared_value), "%s", v->settings.shared[i].value);
            bool is_secret = secret(s->shared_key);
            (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_SHARED_DETAIL_REVEAL_VISIBLE, is_secret);
            TEXT(s, SHARED_DETAIL_KEY, s->shared_key);
            (void)vui_set_text(s->ui, VUI_VIBE_BIND_SHARED_DETAIL_VALUE, (is_secret && !s->shared_detail_reveal) ? "********" : s->shared_value);
            TEXT(s, SHARED_DETAIL_NOTE, "Tap Value to edit this entry.");
            (void)vui_set_text(s->ui, VUI_VIBE_OBJ_KEY_BUTTON_SHARED_DETAIL_DELETE, vibe_translate(s, "Delete entry"));
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
    vibe_show(s, s->wake_page); s->activity_ms = vui_now_ms(s->ui); s->suppress_until = s->activity_ms + 400;
    if (s->page == VIBE_SHARED_EDITOR) project_shared(s);
    haptic(s, 1);
}
static bool pointer_actionable(vibe_ui_t *s, const vui_event_t *e, const char *key)
{
    if (e->type != VUI_EVENT_CALL || !s->pointer_page_ready || s->snapshot.settings.busy) return false;
    if (s->snapshot.updating && !s->snapshot.batch && e->action_id != VUI_VIBE_ACT_ID_CANCEL_DOWNLOAD) return false;
    if (key && !*key) return false;
    if (e->action_id == VUI_VIBE_ACT_ID_SELECT_NETWORKS && e->item >= s->snapshot.ap_count) return false;
    if (e->action_id == VUI_VIBE_ACT_ID_HOME_WIFI && s->snapshot.network == VIBE_NET_CONNECTED) return false;
    return true;
}
static void pointer(vui_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    vibe_ui_t *s = ctx;
    const uint64_t now = vui_now_ms(s->ui);
    s->activity_ms = now;
    if (s->page == VIBE_SLEEP && pressed) { wake(s); s->pointer_down = pressed; return; }
    if (pressed && !s->pointer_down) {
        s->gesture_consumed = false; s->hold_consumed = false; s->touch_haptic_armed = false; s->pointer_page = s->page;
        vui_event_t target = {0};
        const bool has_target = vui_pointer_target(ui, &target);
        const char *key = has_target ? pointer_key(s, &target) : NULL;
        snprintf(s->pointer_key, sizeof(s->pointer_key), "%s", key ? key : "");
        s->pointer_network_open = target.action_id == VUI_VIBE_ACT_ID_SELECT_NETWORKS &&
            target.item < s->snapshot.ap_count && s->snapshot.aps[target.item].open;
        /* Reject a repeated tap on the newly revealed control at the same
         * position. A fresh gesture on another control need not wait. */
        s->pointer_page_ready = now - s->page_enter_ms >= 150 || !s->have_release ||
            now - s->last_release_ms >= 150 || abs(x - s->last_release_x) > 24 ||
            abs(y - s->last_release_y) > 24;
        s->pointer_x = x; s->pointer_y = y;
        if (has_target && now >= s->suppress_until && pointer_actionable(s, &target, key)) {
            s->touch_haptic_armed = true;
            haptic(s, 1);
        }
    }
    if (!pressed && s->pointer_down && s->touch_haptic_armed) {
        s->touch_haptic_armed = false;
        haptic(s, 1);
    }
    if (vui_pointer_moved(ui)) s->gesture_consumed = true;
    if (pressed && !s->pointer_down) {
        s->home_capture = s->page == VIBE_HOME && s->pointer_page_ready && now >= s->suppress_until &&
            x >= VIBE_HOME_DOTS_X && x < VIBE_HOME_DOTS_X + VIBE_HOME_DOTS_SIZE &&
            y >= VIBE_HOME_DOTS_Y && y < VIBE_HOME_DOTS_Y + VIBE_HOME_DOTS_SIZE;
    }
    if (pager_pointer(s, x, y, pressed)) { s->pointer_down = pressed; return; }
    if (s->home_capture) {
        vibe_home_motion_input(&s->home_motion, x - VIBE_HOME_DOTS_X, y - VIBE_HOME_DOTS_Y, pressed, false);
        s->gesture_consumed = true;
        s->pointer_down = pressed;
        if (!pressed) s->home_capture = false;
        return;
    }
    if (s->page == VIBE_RESET || s->page == VIBE_DELETE_CONFIRM) {
        bool inside = x >= 24 && x < 456 && y >= 348 && y < 444;
        bool started_inside = s->pointer_x >= 24 && s->pointer_x < 456 &&
                              s->pointer_y >= 348 && s->pointer_y < 444;
        if (pressed && !s->gesture_consumed && inside && started_inside && s->pointer_page == s->page &&
            s->pointer_page_ready && !s->snapshot.settings.busy) {
            if (!s->hold_started_ms && !s->hold_consumed) s->hold_started_ms = now + 1;
        } else {
            s->hold_started_ms = 0;
            (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, 0);
            if (s->page == VIBE_DELETE_CONFIRM)
                TEXT(s, DELETE_PROGRESS, "Hold for 3 seconds to delete");
        }
    }
    if (!pressed && s->pointer_down && !s->gesture_consumed) {
        s->last_release_ms = now; s->last_release_x = x; s->last_release_y = y; s->have_release = true;
    }
    s->pointer_down = pressed;
}
void vibe_product_tick(vibe_ui_t *s)
{
    const uint64_t now = vui_now_ms(s->ui);
    pager_tick(s->ui, s);
    update_nav_badge(s);
    if (s->undo_until && now >= s->undo_until) {
        if (!commit_delete(s)) { s->undo_until = 0; refresh_library(s); }
    }
    if (s->hold_started_ms && now + 1 >= s->hold_started_ms) {
        unsigned elapsed = (unsigned)(now + 1 - s->hold_started_ms);
        if (s->page == VIBE_RESET) (void)vui_set_value(s->ui, VUI_VIBE_BIND_RESET_HOLD_PROGRESS, elapsed >= 3000 ? 100 : elapsed / 30);
        else if (s->page == VIBE_DELETE_CONFIRM) FORMAT(s, DELETE_PROGRESS, "Hold · %u s", elapsed >= 3000 ? 0 : (3000 - elapsed + 999) / 1000);
        if (elapsed >= 3000) {
            s->hold_started_ms = 0;
            s->hold_consumed = true;
            if (s->page == VIBE_RESET && !s->snapshot.idea_busy && !s->snapshot.updating) (void)setting(s, VIBE_SETTINGS_RESET, NULL, NULL);
            else if (s->page == VIBE_DELETE_CONFIRM) {
                s->undo_shared = !s->delete_code[0];
                s->undo_until = now + 4000;
                memset(s->shared_value, 0, sizeof(s->shared_value));
                refresh_library(s); vibe_show(s, s->undo_shared ? VIBE_SHARED : VIBE_NAND);
                haptic(s, 2);
            }
        }
    }
    if (s->services.key) {
        unsigned key;
        while ((key = s->services.key(s->services.ctx)) != 0) {
            s->activity_ms = now;
            vibe_product_cancel_pager(s);
            if (key == 1) {
                haptic(s, 1);
                if (s->page == VIBE_SLEEP) wake(s);
                else if (s->page == VIBE_FORMAT_NAND_CONFIRM) vibe_show(s, s->format_return);
                else if (s->page == VIBE_UPDATE_CONFIRM) vibe_show(s, VIBE_NAND);
                else if (s->page == VIBE_OPEN_CONFIRM || s->page == VIBE_GUIDE) vibe_show(s, s->return_page);
                else if (s->page != VIBE_HOME && s->page != VIBE_GRID &&
                         !s->snapshot.updating && !s->snapshot.idea_busy && !s->preview_active) vibe_show(s, VIBE_HOME);
            } else if (key == 2) {
                haptic(s, 1);
            } else if (key == 3 && !s->snapshot.updating) {
                haptic(s, 1);
                if (s->page == VIBE_SLEEP) wake(s);
                else { s->boot_page = s->page; vibe_show(s, VIBE_BOOT_TIP); }
            } else if (key == 4) {
                haptic(s, 1);
                if (s->page == VIBE_BOOT_TIP) {
                    vibe_show(s, s->boot_page);
                    if (s->page == VIBE_SHARED_EDITOR) project_shared(s);
                } else if (s->page == VIBE_SLEEP && s->wake_page == VIBE_BOOT_TIP)
                    s->wake_page = s->boot_page;
            }
        }
    }
    s->motion_revision = s->snapshot.device.motion_revision;
    unsigned timeout = s->snapshot.settings.screen_off_seconds;
    if (timeout && s->page != VIBE_SLEEP && s->page != VIBE_WELCOME &&
        now - s->activity_ms >= (uint64_t)timeout * 1000) sleep_now(s);
    if (s->services.device_policy) s->services.device_policy(s->services.ctx, s->page == VIBE_SLEEP,
        !s->pointer_down && !s->pager.active && !s->hold_started_ms && s->page != VIBE_PASSWORD && s->page != VIBE_SHARED_EDITOR &&
        !s->snapshot.updating && s->page != VIBE_BOOT_TIP && s->page != VIBE_OPEN_CONFIRM);
}
void vibe_product_init(vibe_ui_t *s)
{
    s->wifi_return = VIBE_SETTINGS;
    (void)vui_set_pointer_observer(s->ui, pointer, s);
    s->first_run = s->snapshot.settings.language == 0;
    if (s->first_run) vibe_show(s, VIBE_WELCOME);
}
