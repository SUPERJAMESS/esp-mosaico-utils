// SPDX-License-Identifier: Apache-2.0
#include "vibe_ui.h"
#include "vibe_ui_state.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define VIBE_STATE_STORAGE EXT_RAM_BSS_ATTR
#else
#define VIBE_STATE_STORAGE
#endif
static VIBE_STATE_STORAGE vibe_ui_t s_controller;
#include "ideas_presentation.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vui_ids.h"

#define TEXT(s, name, value) vui_set_text((s)->ui, VUI_VIBE_BIND_##name, vibe_translate(s, value))
static void format(vibe_ui_t *s, uint16_t bind, const char *fmt, ...)
{
    char text[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), vibe_translate(s, fmt), args);
    va_end(args);
    (void)vui_set_text(s->ui, bind, text);
}
#define FORMAT(s, name, ...) format(s, VUI_VIBE_BIND_##name, __VA_ARGS__)
static void transfer_text(vibe_ui_t *s)
{
    const vibe_snapshot_t *v = &s->snapshot;
    const vibe_transfer_t *t = &v->transfer;
    const uint64_t unit = (t->total ? t->total : t->received) >= 1024 * 1024 ? 1024 * 1024 : 1024;
    const char *suffix = unit == 1024 ? "KiB" : "MiB";
    const unsigned received = (unsigned)(t->received * 100 / unit);
    const unsigned total = (unsigned)(t->total * 100 / unit);
    if (t->total) {
        FORMAT(s, UPDATE_PERCENT, "%u%%", vibe_transfer_permille(t->received, t->total) / 10);
        FORMAT(s, UPDATE_BYTES, "%u.%02u / %u.%02u %s", received / 100, received % 100, total / 100,
               total % 100, suffix);
    } else {
        TEXT(s, UPDATE_PERCENT, "--");
        FORMAT(s, UPDATE_BYTES, "%u.%02u %s / --", received / 100, received % 100, suffix);
    }
    (void)vui_set_value(s->ui, VUI_VIBE_BIND_PROGRESS,
                            vibe_transfer_permille(t->received, t->total) / 10);
    const char *verb = (!strcmp(v->update_owner, "Bridge") || !strcmp(v->update_owner, "Cloud"))
                           ? "Download"
                       : !strcmp(v->update_owner, "NAND") ? "Read"
                                                          : "Receive";
    if (!s->rate.valid)
        FORMAT(s, UPDATE_RATE, "%s: --", verb);
    else {
        const uint64_t rate_unit = s->rate.bytes_per_second >= 1024 * 1024 ? 1024 * 1024 : 1024;
        const unsigned tenths = (unsigned)(s->rate.bytes_per_second * 10 / rate_unit);
        FORMAT(s, UPDATE_RATE, "%s: %u.%u %s/s", verb, tenths / 10, tenths % 10,
               rate_unit == 1024 ? "KiB" : "MiB");
    }
    if (t->component_count && t->component_id)
        FORMAT(s, UPDATE_COMPONENT, "Component %u: %u%% | %u/%u verified", t->component_id,
               vibe_transfer_permille(t->component_received, t->component_size) / 10,
               t->completed_components, t->component_count);
    else if (t->component_count)
        FORMAT(s, UPDATE_COMPONENT, "%u/%u components verified", t->completed_components,
               t->component_count);
    else
        TEXT(s, UPDATE_COMPONENT, "Application image");
}
static int command(vibe_ui_t *s, vibe_command_t cmd, const char *a, const char *b)
{
    return s->services.command(s->services.ctx, cmd, a, b);
}
static void clear_secret(char *text, size_t size)
{
    volatile char *p = text;
    while (size--)
        *p++ = 0;
}
static void clear_preview(vibe_ui_t *s)
{
    (void)vui_canvas_stop(s->ui, VUI_VIBE_BIND_IDEA_TITLE_PIXELS);
    (void)vui_canvas_stop(s->ui, VUI_VIBE_BIND_IDEA_BODY_PIXELS);
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_TITLE_PIXELS, false);
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_BODY_PIXELS, false);
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_ICON, false);
    s->rendered_preview = 0;
}
static int render_preview(vibe_ui_t *s)
{
    const vibe_snapshot_t *v = &s->snapshot;
    if (s->rendered_preview == v->idea_request)
        return 0;
    if ((v->idea_custom || v->idea_has_icon) && !s->services.preview)
        return VUI_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 3; ++i) {
        if (i < 2 ? !v->idea_custom : !v->idea_has_icon)
            continue;
        size_t size = 0;
        void *data = s->services.preview(s->services.ctx, v->idea_request, i, &size);
        if (!data)
            return VUI_ERR_NO_MEM;
        int err;
        if (i < 2) {
            size_t width = i ? IDEAS_DESCRIPTION_WIDTH : IDEAS_TITLE_WIDTH;
            size_t height = i ? IDEAS_DESCRIPTION_HEIGHT : IDEAS_TITLE_HEIGHT;
            err = size == width * height * 2
                      ? vui_canvas_push(s->ui,
                                            i ? VUI_VIBE_BIND_IDEA_BODY_PIXELS
                                              : VUI_VIBE_BIND_IDEA_TITLE_PIXELS,
                                            data, width * 2, free, data)
                      : VUI_ERR_INVALID_ARG;
        } else
            err = vui_set_image_owned(s->ui, VUI_VIBE_BIND_IDEA_ICON, data, size);
        if (err) {
            free(data);
            clear_preview(s);
            return err;
        }
    }
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_TITLE_PIXELS, v->idea_custom);
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_BODY_PIXELS, v->idea_custom);
    (void)vui_component_set_visible(s->ui, VUI_VIBE_OBJ_KEY_IDEA_ICON, v->idea_has_icon);
    s->rendered_preview = v->idea_request;
    return 0;
}
static void show(vibe_ui_t *s, vibe_page_t page)
{
    static const uint16_t visibility[VIBE_PAGE_COUNT] = {
        VUI_VIBE_BIND_HOME_VISIBLE,     VUI_VIBE_BIND_WIFI_VISIBLE,
        VUI_VIBE_BIND_PASSWORD_VISIBLE, VUI_VIBE_BIND_PAIRING_VISIBLE,
        VUI_VIBE_BIND_BRIDGE_VISIBLE,   VUI_VIBE_BIND_NAND_VISIBLE,
        VUI_VIBE_BIND_CONFIRM_VISIBLE,  VUI_VIBE_BIND_UPDATE_VISIBLE,
        VUI_VIBE_BIND_RESULT_VISIBLE,   VUI_VIBE_BIND_IDEA_INFO_VISIBLE,
        VUI_VIBE_BIND_DEVELOP_VISIBLE, VUI_VIBE_BIND_SETTINGS_VISIBLE,
        VUI_VIBE_BIND_DISPLAY_VISIBLE, VUI_VIBE_BIND_LANGUAGE_VISIBLE,
        VUI_VIBE_BIND_SHARED_VISIBLE, VUI_VIBE_BIND_SHARED_EDITOR_VISIBLE,
        VUI_VIBE_BIND_ABOUT_VISIBLE, VUI_VIBE_BIND_RESET_VISIBLE,
        VUI_VIBE_BIND_WELCOME_VISIBLE, VUI_VIBE_BIND_GRID_VISIBLE,
        VUI_VIBE_BIND_OPEN_CONFIRM_VISIBLE, VUI_VIBE_BIND_SLEEP_VISIBLE,
        VUI_VIBE_BIND_BOOT_TIP_VISIBLE, VUI_VIBE_BIND_DELETE_CONFIRM_VISIBLE,
        VUI_VIBE_BIND_HELP_VISIBLE, VUI_VIBE_BIND_GUIDE_VISIBLE,
    };
    if (!vibe_product_leave(s, page)) return;
    const vibe_page_t previous = s->page;
    if (previous == VIBE_IDEA_INFO && page != VIBE_IDEA_INFO)
        clear_preview(s);
    if (previous == VIBE_PASSWORD && page != VIBE_PASSWORD) {
        clear_secret(s->password, sizeof(s->password));
        TEXT(s, PASSWORD_DISPLAY, "Enter password");
        clear_secret(s->password_projection, sizeof(s->password_projection));
        s->password_visible = false;
        TEXT(s, PASSWORD_TOGGLE, "Show");
    }
    if (page != VIBE_WIFI && page != VIBE_PASSWORD)
        s->download_pending = false;
    if (previous != page) s->page_enter_tick = s->tick;
    s->page = page;
    /* Authored layers start with only HOME visible. Update only the two
     * participating layers, avoiding redundant visibility invalidations. */
    if (previous != page)
        (void)vui_set_visible(s->ui, visibility[previous], false);
    (void)vui_set_visible(s->ui, visibility[page], true);
    if (page == VIBE_WIFI) {
        TEXT(s, WIFI_NOTE,
             s->download_pending ? "Connect Wi-Fi to download ideas"
                                 : "USB remains available offline");
        (void)vui_list_refresh(s->ui, s->networks);
        if (previous != page)
            (void)command(s, VIBE_SCAN_WIFI, NULL, NULL);
    }
    (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUNDLES_VISIBLE, page == VIBE_NAND);
    if (page == VIBE_NAND)
        (void)vui_list_refresh(s->ui, s->bundles);
}
static void error_result(vibe_ui_t *s, const char *title, int error)
{
    TEXT(s, RESULT_TITLE, title);
    FORMAT(s, RESULT_DETAIL, "Error 0x%08x\nReturn home and retry", (unsigned)error);
    show(s, VIBE_RESULT);
}
int vibe_ui_open_bridge(vibe_ui_t *s)
{
    if (!s || !s->ui)
        return VUI_ERR_INVALID_ARG;
    s->services.snapshot(s->services.ctx, &s->snapshot);
    if (s->preview_active || s->snapshot.idea_busy || s->snapshot.idea_ready ||
        s->snapshot.updating)
        return VUI_ERR_INVALID_STATE;
    memset(s->idea_code, 0, sizeof(s->idea_code));
    show(s, VIBE_BRIDGE);
    vibe_ui_poll(s);
    return 0;
}
static vui_err_t network_row(vui_handle_t ui, vui_row_t row, uint32_t index, void *ctx)
{
    vibe_ui_t *s = ctx;
    if (index >= s->snapshot.ap_count)
        return VUI_ERR_INVALID_ARG;
    const vibe_ap_t *ap = &s->snapshot.aps[index];
    char detail[48];
    snprintf(detail, sizeof(detail), "%s - %d dBm", ap->open ? "Open" : "Secured", ap->rssi);
    vui_err_t err = vui_vibe_networks_row_row_set_networks_title_text(ui, row, ap->ssid);
    if (err == VUI_OK)
        err = vui_vibe_networks_row_row_set_networks_detail_text(ui, row, detail);
    return err == VUI_OK ? VUI_OK : VUI_ERR_INVALID_STATE;
}
static bool ascii_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p > 126)
            return false;
    return true;
}
static vui_err_t bundle_row(vui_handle_t ui, vui_row_t row, uint32_t index, void *ctx)
{
    vibe_ui_t *s = ctx;
    index = vibe_bundle_index(s, index);
    if (index >= s->snapshot.bundle_count)
        return VUI_ERR_INVALID_ARG;
    const vibe_bundle_t *bundle = &s->snapshot.bundles[index];
    char detail[128];
    snprintf(detail, sizeof(detail), "%s", bundle->boot ? vibe_translate(s, "Current")
             : bundle->update_available ? vibe_translate(s, "Update available") : bundle->version);
    bool hot = bundle->boot || bundle->update_available;
    if (s->snapshot.batch && !strcmp(bundle->code, s->snapshot.batch_code)) {
        hot = true;
        snprintf(detail, sizeof(detail), vibe_translate(s, "Updating · %u%%"),
                 vibe_transfer_permille(s->snapshot.transfer.received, s->snapshot.transfer.total) / 10);
    } else if (s->snapshot.batch && bundle->queued) {
        hot = false;
        snprintf(detail, sizeof(detail), "%s", vibe_translate(s, "Waiting"));
    }
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "Idea %s", bundle->code);
    vui_err_t err = vui_vibe_bundles_row_row_set_bundles_detail_text(ui, row, detail);
    if (err == VUI_OK)
        err = vui_row_set_color(ui, row, VUI_VIBE_TEMPLATE_BUNDLES_ROW_BUNDLES_DETAIL_FG_COLOR_SLOT, hot ? 0xfa60 : 0x9cd3);
    if (err == VUI_OK)
        err = vui_vibe_bundles_row_row_set_bundles_title_text(
            ui, row, ascii_text(bundle->release) ? bundle->release : fallback);
    if (s->services.icon) {
        size_t size = 0;
        void *data = s->services.icon(s->services.ctx, bundle->code, &size);
        if (data && vui_row_set_image_owned(
                        ui, row, VUI_VIBE_TEMPLATE_BUNDLES_ROW_BUNDLES_ICON_RESOURCE_SLOT, data,
                        size) != VUI_OK)
            free(data);
    }
    return err == VUI_OK ? VUI_OK : VUI_ERR_INVALID_STATE;
}
static void project_password(vibe_ui_t *s)
{
    char password[65] = {0}, display[65] = {0};
    memcpy(password, s->password, sizeof(password));
    const size_t n = strlen(password);
    if (s->password_visible)
        memcpy(display, password, n);
    else
        memset(display, '*', n);
    if (strcmp(display, s->password_projection) != 0) {
        memcpy(s->password_projection, display, sizeof(display));
        /* Keep the tail visible when a long password exceeds the field. */
        TEXT(s, PASSWORD_DISPLAY, n ? display + (n > 22 ? n - 22 : 0) : "Enter password");
    }
    clear_secret(password, sizeof(password));
    clear_secret(display, sizeof(display));
}
static void submit_password(vibe_ui_t *s)
{
    char password[65] = {0};
    memcpy(password, s->password, sizeof(password));
    int err = command(s, VIBE_CONNECT_WIFI, s->selected_ssid, password);
    clear_secret(password, sizeof(password));
    if (err)
        TEXT(s, PASSWORD_ERROR, "Check password and try again");
    else
        show(s, s->download_pending || s->first_run ? VIBE_WIFI : VIBE_HOME);
}
static void prepare_idea(vibe_ui_t *s, vibe_command_t cmd, const char *code)
{
    s->services.snapshot(s->services.ctx, &s->snapshot);
    if (s->snapshot.idea_busy)
        return;
    int err = command(s, cmd, code, NULL);
    if (err) {
        error_result(s, "App information failed", err);
        return;
    }
    s->preview_return = s->page;
    s->preview_active = true;
    show(s, VIBE_IDEA_INFO);
    vibe_ui_poll(s);
}
static void event(vui_handle_t ui, const vui_event_t *e, void *ctx)
{
    vibe_ui_t *s = ctx;
    if (e->type != VUI_EVENT_CALL)
        return;
    s->activity_tick = s->tick;
    if (vibe_product_event(s, e))
        return;
#ifdef VIBE_UI_TRACE
    fprintf(stderr, "vibe_event: action=%u list=%u item=%u page=%d networks=%u bundles=%u\n",
            e->action_id, e->list, (unsigned)e->item, s->page, s->networks, s->bundles);
#endif
    if (s->snapshot.updating && !s->snapshot.batch)
        return;
    int err;
    switch (e->action_id) {
    case VUI_VIBE_ACT_ID_DOWNLOAD:
        if (s->snapshot.credentials_saved || s->snapshot.network == VIBE_NET_CONNECTED) {
            (void)vibe_ui_open_bridge(s);
        } else {
            s->download_pending = true;
            show(s, VIBE_WIFI);
        }
        break;
    case VUI_VIBE_ACT_ID_WIFI:
        s->download_pending = false;
        show(s, VIBE_WIFI);
        break;
    case VUI_VIBE_ACT_ID_PAIRING:
        show(s, VIBE_PAIRING);
        break;
    case VUI_VIBE_ACT_ID_BACK_PASSWORD:
        show(s, VIBE_WIFI);
        break;
    case VUI_VIBE_ACT_ID_BACK_CONFIRM:
    case VUI_VIBE_ACT_ID_CANCEL_INSTALL:
        show(s, VIBE_NAND);
        break;
    case VUI_VIBE_ACT_ID_HOME:
    case VUI_VIBE_ACT_ID_BACK_WIFI:
    case VUI_VIBE_ACT_ID_BACK_PAIRING:
    case VUI_VIBE_ACT_ID_BACK_NAND:
    case VUI_VIBE_ACT_ID_BACK_BRIDGE:
        s->acknowledged_update = s->snapshot.update_revision;
        show(s, VIBE_HOME);
        break;
    case VUI_VIBE_ACT_ID_SCAN:
        err = command(s, VIBE_SCAN_WIFI, NULL, NULL);
        if (err)
            FORMAT(s, WIFI_SCAN, "Scan error 0x%x", (unsigned)err);
        break;
    case VUI_VIBE_ACT_ID_FORGET:
        (void)command(s, VIBE_STOP_BRIDGE, NULL, NULL);
        s->prefetch_attempted = false;
        (void)command(s, VIBE_FORGET_WIFI, NULL, NULL);
        break;
    case VUI_VIBE_ACT_ID_NAND:
        show(s, VIBE_NAND);
        err = command(s, VIBE_SCAN_NAND, NULL, NULL);
        if (err)
            FORMAT(s, NAND_SCAN, "Scan error 0x%x", (unsigned)err);
        break;
    case VUI_VIBE_ACT_ID_SELECT_NETWORKS:
        if (s->page != VIBE_WIFI || e->list != s->networks || e->item >= s->snapshot.ap_count)
            break;
        snprintf(s->selected_ssid, sizeof(s->selected_ssid), "%s", s->snapshot.aps[e->item].ssid);
        if (s->snapshot.aps[e->item].open) {
            err = command(s, VIBE_CONNECT_WIFI, s->selected_ssid, "");
            if (err) error_result(s, "Connection failed", err);
            break;
        }
        TEXT(s, SSID, s->selected_ssid);
        TEXT(s, PASSWORD_ERROR, "");
        clear_secret(s->password, sizeof(s->password));
        TEXT(s, PASSWORD_DISPLAY, "Enter password");
        s->password_visible = false;
        clear_secret(s->password_projection, sizeof(s->password_projection));
        show(s, VIBE_PASSWORD);
        (void)vui_component_set_visible(ui, VUI_VIBE_OBJ_KEY_PASSWORD_KEYBOARD_LOWER, true);
        (void)vui_component_set_visible(ui, VUI_VIBE_OBJ_KEY_PASSWORD_KEYBOARD_UPPER, false);
        (void)vui_component_set_visible(ui, VUI_VIBE_OBJ_KEY_PASSWORD_KEYBOARD_SYM, false);
        break;
    case VUI_VIBE_ACT_ID_PASSWORD_KEYBOARD_KEY:
        if (s->page != VIBE_PASSWORD)
            break;
        /* Vibe keyboard keys emit Unicode codepoints, Backspace=8, Enter=13.
         * Own the bounded buffer so masking works identically with 1.2.0's
         * device runtime and sim_bridge (which lacks keyboard read-back). */
        if (e->arg == 13)
            submit_password(s);
        else {
            size_t len = strlen(s->password);
            if (e->arg == 8 && len)
                s->password[len - 1] = 0;
            else if (e->arg >= 32 && e->arg <= 126 && len < 64) {
                s->password[len] = (char)e->arg;
                s->password[len + 1] = 0;
            }
            project_password(s);
        }
        break;
    case VUI_VIBE_ACT_ID_TOGGLE_PASSWORD:
        if (s->page == VIBE_PASSWORD) {
            s->password_visible = !s->password_visible;
            TEXT(s, PASSWORD_TOGGLE, s->password_visible ? "Hide" : "Show");
            project_password(s);
        }
        break;
    case VUI_VIBE_ACT_ID_SELECT_BUNDLES:
        if (s->page != VIBE_NAND || e->list != s->bundles || e->item >= s->snapshot.bundle_count)
            break;
        if (vibe_bundle_index(s, e->item) >= s->snapshot.bundle_count) break;
        s->selected_bundle = s->snapshot.bundles[vibe_bundle_index(s, e->item)];
        if (ascii_text(s->selected_bundle.release))
            TEXT(s, RELEASE, s->selected_bundle.release);
        else
            FORMAT(s, RELEASE, "Idea %s", s->selected_bundle.code);
        FORMAT(s, RELEASE_DETAIL, "%s | %.40s%s", s->selected_bundle.code,
               s->selected_bundle.version, s->selected_bundle.boot ? " | Boot app" : "");
        TEXT(s, RELEASE_PATH,
             ascii_text(s->selected_bundle.description) ? s->selected_bundle.description
                                                        : "View info for name and description");
        (void)vui_set_visible(ui, VUI_VIBE_BIND_BUTTON_START_IDEA_VISIBLE,
                                            !s->selected_bundle.boot);
        (void)vui_set_visible(ui, VUI_VIBE_BIND_BUTTON_CHECK_IDEA_VISIBLE, s->selected_bundle.code[0] != 'd');
        (void)vui_set_visible(ui, VUI_VIBE_BIND_BUTTON_UPDATE_IDEA_VISIBLE, false);
        show(s, VIBE_CONFIRM);
        break;
    case VUI_VIBE_ACT_ID_INSTALL:
        if (s->page != VIBE_CONFIRM)
            break;
        err = command(s, VIBE_INSTALL_NAND, s->selected_bundle.code, NULL);
        if (err)
            error_result(s, "Installation failed", err);
        else {
            TEXT(s, UPDATE_TITLE, "Setting boot application");
            TEXT(s, UPDATE_DETAIL, "Verifying downloaded files");
            TEXT(s, UPDATE_PERCENT, "0%");
            TEXT(s, UPDATE_OWNER, "NAND");
            TEXT(s, UPDATE_VERIFIED, "Validating");
            (void)vui_set_value(ui, VUI_VIBE_BIND_PROGRESS, 0);
            show(s, VIBE_UPDATE);
        }
        break;
    case VUI_VIBE_ACT_ID_CODE_0:
    case VUI_VIBE_ACT_ID_CODE_1:
    case VUI_VIBE_ACT_ID_CODE_2:
    case VUI_VIBE_ACT_ID_CODE_3:
    case VUI_VIBE_ACT_ID_CODE_4:
    case VUI_VIBE_ACT_ID_CODE_5:
    case VUI_VIBE_ACT_ID_CODE_6:
    case VUI_VIBE_ACT_ID_CODE_7:
    case VUI_VIBE_ACT_ID_CODE_8:
    case VUI_VIBE_ACT_ID_CODE_9:
        if (s->page == VIBE_BRIDGE && strlen(s->idea_code) < 6) {
            const uint16_t ids[] = {VUI_VIBE_ACT_ID_CODE_0, VUI_VIBE_ACT_ID_CODE_1,
                                    VUI_VIBE_ACT_ID_CODE_2, VUI_VIBE_ACT_ID_CODE_3,
                                    VUI_VIBE_ACT_ID_CODE_4, VUI_VIBE_ACT_ID_CODE_5,
                                    VUI_VIBE_ACT_ID_CODE_6, VUI_VIBE_ACT_ID_CODE_7,
                                    VUI_VIBE_ACT_ID_CODE_8, VUI_VIBE_ACT_ID_CODE_9};
            for (unsigned i = 0; i < 10; i++)
                if (ids[i] == e->action_id) {
                    size_t n = strlen(s->idea_code);
                    s->idea_code[n] = '0' + i;
                    s->idea_code[n + 1] = 0;
                    break;
                }
            vibe_ui_poll(s);
        }
        break;
    case VUI_VIBE_ACT_ID_CODE_DELETE:
        if (s->page == VIBE_BRIDGE && s->idea_code[0]) {
            s->idea_code[strlen(s->idea_code) - 1] = 0;
            vibe_ui_poll(s);
        }
        break;
    case VUI_VIBE_ACT_ID_DOWNLOAD_CODE:
        if (s->page != VIBE_BRIDGE)
            break;
        if (strlen(s->idea_code) != 6) {
            TEXT(s, BRIDGE_DETAIL, "Enter all six digits");
            break;
        }
        prepare_idea(s, VIBE_RESOLVE_IDEA, s->idea_code);
        break;
    case VUI_VIBE_ACT_ID_CHECK_IDEA:
    case VUI_VIBE_ACT_ID_UPDATE_IDEA:
    case VUI_VIBE_ACT_ID_PREVIEW_IDEA:
        if (s->page != VIBE_CONFIRM)
            break;
        prepare_idea(s,
                     e->action_id == VUI_VIBE_ACT_ID_PREVIEW_IDEA ? VIBE_PREVIEW_IDEA
                     : e->action_id == VUI_VIBE_ACT_ID_CHECK_IDEA ? VIBE_CHECK_IDEA
                                                                  : VIBE_RESOLVE_IDEA,
                     s->selected_bundle.code);
        break;
    case VUI_VIBE_ACT_ID_CANCEL_IDEA:
        if (s->page != VIBE_IDEA_INFO || !s->preview_active)
            break;
        err = command(s, VIBE_CANCEL_IDEA, NULL, NULL);
        if (!err) {
            s->preview_active = false;
            show(s, s->preview_return);
        }
        break;
    case VUI_VIBE_ACT_ID_ACCEPT_IDEA:
        if (s->page != VIBE_IDEA_INFO || !s->preview_active || !s->snapshot.idea_ready ||
            s->snapshot.idea_view_only)
            break;
        {
            char request[16];
            snprintf(request, sizeof(request), "%lu", (unsigned long)s->snapshot.idea_request);
            s->selected_bundle = s->snapshot.idea;
            err = command(s, VIBE_DOWNLOAD_IDEA, s->snapshot.idea.code, request);
            if (!err)
                vibe_ui_poll(s);
        }
        break;
    default:
        break;
    }
}
void vibe_ui_poll(vibe_ui_t *s)
{
    uint32_t scan = s->snapshot.scan_generation, nand = s->snapshot.nand_generation;
    size_t ap_count = s->snapshot.ap_count, bundle_count = s->snapshot.bundle_count;
    bool batch = s->snapshot.batch;
    unsigned batch_progress = vibe_transfer_permille(s->snapshot.transfer.received, s->snapshot.transfer.total) / 10;
    s->services.snapshot(s->services.ctx, &s->snapshot);
    vibe_snapshot_t *v = &s->snapshot;
    vibe_rate_sample(&s->rate, &v->transfer);
    if (v->ap_count > VIBE_AP_MAX)
        v->ap_count = VIBE_AP_MAX;
    if (v->bundle_count > VIBE_BUNDLE_MAX)
        v->bundle_count = VIBE_BUNDLE_MAX;
    if (scan != v->scan_generation || ap_count != v->ap_count) {
        (void)vui_list_set_total(s->ui, s->networks, v->ap_count);
        (void)vui_list_refresh(s->ui, s->networks);
    }
    if (nand != v->nand_generation || bundle_count != v->bundle_count || batch != v->batch ||
        (v->batch && batch_progress != vibe_transfer_permille(v->transfer.received, v->transfer.total) / 10)) {
        (void)vui_list_set_total(s->ui, s->bundles, vibe_bundle_count(s));
        (void)vui_list_refresh(s->ui, s->bundles);
        /* Invalidate the retained viewport after a rebind. Both mutations run
         * in this render callback, preserving scroll position without painting
         * an intermediate hidden frame on the device. */
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUNDLES_VISIBLE, false);
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUNDLES_VISIBLE, s->page == VIBE_NAND);
    }
    if (s->preview_active && !v->idea_loading && !v->idea_ready) {
        s->preview_active = false;
        show(s, v->idea_return_library ? VIBE_NAND : s->preview_return);
    }
    if (v->updating && !v->batch) {
        if (s->page != VIBE_UPDATE)
            show(s, VIBE_UPDATE);
        TEXT(s, UPDATE_TITLE, v->update_title);
        TEXT(s, UPDATE_DETAIL, v->update_detail);
        TEXT(s, UPDATE_OWNER, v->update_owner);
        TEXT(s, UPDATE_VERIFIED, v->update_verified);
        transfer_text(s);
    } else if (!v->updating && v->update_terminal && v->update_revision != s->acknowledged_update) {
        s->acknowledged_update = v->update_revision;
        TEXT(s, RESULT_TITLE, v->update_title);
        if (v->update_failed && v->transfer.total)
            FORMAT(s, RESULT_DETAIL, "%s\nTransferred: %u%%", v->update_detail,
                   vibe_transfer_permille(v->transfer.received, v->transfer.total) / 10);
        else
            TEXT(s, RESULT_DETAIL, v->update_detail);
        /* Explicit batch cancellation belongs to the library. A terminal
         * snapshot may arrive after the input callback, so acknowledge it
         * here without a delayed jump to the result page. Surface failures. */
        if (!s->batch_cancel_pending || v->update_failed)
            show(s, VIBE_RESULT);
        s->batch_cancel_pending = false;
    }
    switch (s->page) {
    case VIBE_HOME:
        TEXT(s, USB_STATUS, v->usb_owner ? "Active" : v->session ? "Paused" : "Available");
        TEXT(s, NETWORK_STATUS,
             v->network == VIBE_NET_CONNECTED    ? (v->tcp_owner ? "Active"
                                                    : v->session ? "Paused"
                                                                 : "Available")
             : v->network == VIBE_NET_CONNECTING ? "Connecting"
                                                 : "Offline");
        if (v->network == VIBE_NET_CONNECTED)
            FORMAT(s, ADDRESS, "%s.local - %s:%u", v->hostname, v->ip, v->tcp_port);
        else
            TEXT(s, ADDRESS, "Network setup not completed");
        break;
    case VIBE_WIFI:
        TEXT(s, WIFI_CONNECTION,
             v->network == VIBE_NET_CONNECTED || v->network == VIBE_NET_CONNECTING ? v->ssid
             : v->network == VIBE_NET_FAILED ? "Connection failed"
                                             : "Not connected");
        if (v->network == VIBE_NET_CONNECTED)
            FORMAT(s, WIFI_DETAIL, "%s", v->ip);
        else
            TEXT(s, WIFI_DETAIL,
                 v->network == VIBE_NET_CONNECTING ? "Connecting..." : "Choose a network below");
        if (v->scanning)
            TEXT(s, WIFI_SCAN, "Scanning...");
        else
            FORMAT(s, WIFI_SCAN, "%u networks found", (unsigned)v->ap_count);
        if (s->download_pending && v->network == VIBE_NET_CONNECTED && v->ip[0])
            (void)vibe_ui_open_bridge(s);
        break;
    case VIBE_PAIRING:
        if (v->network == VIBE_NET_CONNECTED)
            FORMAT(s, PAIRING_ENDPOINT, "%s.local\n%s:%u", v->hostname, v->ip, v->tcp_port);
        else
            TEXT(s, PAIRING_ENDPOINT, "Connect Wi-Fi first");
        if (v->token[0])
            FORMAT(s, PAIRING_TOKEN, "%.32s\n%.32s", v->token, v->token + 32);
        else
            TEXT(s, PAIRING_TOKEN, "Pairing token unavailable");
        break;
    case VIBE_BRIDGE: {
        char display[7] = "------";
        memcpy(display, s->idea_code, strlen(s->idea_code));
        TEXT(s, BRIDGE_CODE, display);
        TEXT(s, BRIDGE_DETAIL,
             v->network == VIBE_NET_CONNECTED ? (v->idea_busy ? "Finishing previous request..."
                                                              : "Enter the six-digit Idea code")
                                              : "Wi-Fi is not connected");
    } break;
    case VIBE_IDEA_INFO:
        if (v->idea_ready) {
            int err = render_preview(s);
            if (err) {
                (void)command(s, VIBE_CANCEL_IDEA, NULL, NULL);
                s->preview_active = false;
                error_result(s, "App information failed", err);
                break;
            }
        }
        TEXT(s, IDEA_NAME,
             !v->idea_ready   ? "Loading app information..."
             : v->idea_custom ? ""
                              : v->idea.release);
        TEXT(s, IDEA_DESCRIPTION,
             !v->idea_ready   ? (v->idea_view_only ? "Reading downloaded resources"
                                                   : "Fetching details from the cloud")
             : v->idea_custom ? ""
                              : v->idea.description);
        if (v->idea_ready)
            FORMAT(s, IDEA_SUMMARY, "%s | %.32s | %lu KiB", v->idea.code, v->idea.version,
                   (unsigned long)((v->idea.bytes + 1023) / 1024));
        else
            TEXT(s, IDEA_SUMMARY, "");
        TEXT(s, IDEA_STATUS,
             !v->idea_ready       ? "Please wait"
             : v->idea_view_only  ? "Saved in My Ideas"
             : v->idea_downloaded ? "Already downloaded"
                                  : "Confirm to save in My Ideas");
        TEXT(s, IDEA_ACCEPT, v->idea_downloaded ? "My Ideas" : "Download");
        (void)vui_set_visible(s->ui, VUI_VIBE_BIND_BUTTON_ACCEPT_IDEA_VISIBLE,
                                            v->idea_ready && !v->idea_view_only);
        break;
    default:
        break;
    }
    vibe_product_poll(s);
}
static void tick(vui_handle_t ui, void *ctx)
{
    (void)ui;
    vibe_ui_t *s = ctx;
    vibe_product_tick(s);
    if (s->page == VIBE_PASSWORD)
        project_password(s);
    if (++s->tick % 5 == 0)
        vibe_ui_poll(s);
}
vui_err_t vibe_ui_create(vui_handle_t ui, const vibe_services_t *services, vibe_ui_t **out)
{
    if (!out)
        return VUI_ERR_INVALID_ARG;
    *out = NULL;
    vibe_ui_t *s = &s_controller;
    if (s->ui)
        return VUI_ERR_INVALID_STATE;
    if (!s || !ui || !services || !services->snapshot || !services->command)
        return VUI_ERR_INVALID_ARG;
    *out = s;
    memset(s, 0, sizeof(*s));
    s->ui = ui;
    s->services = *services;
    s->last_language = 3;
    s->networks = vui_list_bind_component(ui, VUI_VIBE_OBJ_KEY_NETWORKS, network_row, s);
    s->bundles = vui_list_bind_component(ui, VUI_VIBE_OBJ_KEY_BUNDLES, bundle_row, s);
    if (s->networks == VUI_LIST_NONE || s->bundles == VUI_LIST_NONE)
        return VUI_ERR_NO_MEM;
    vui_err_t err = vui_on_event(ui, event, s);
    if (err != VUI_OK)
        return err;
    show(s, VIBE_HOME);
    vibe_ui_poll(s);
    vibe_product_init(s);
    s->timer = vui_timer_create(ui, 50, tick, s);
    return s->timer ? VUI_OK : VUI_ERR_NO_MEM;
}
void vibe_ui_deinit(vibe_ui_t *s)
{
    if (!s || !s->ui)
        return;
    show(s, VIBE_HOME);
    (void)command(s, VIBE_STOP_BRIDGE, NULL, NULL);
    if (s->timer)
        (void)vui_timer_delete(s->ui, s->timer);
    (void)vui_on_event(s->ui, NULL, NULL);
    (void)vui_set_pointer_observer(s->ui, NULL, NULL);
    clear_secret(s->snapshot.token, sizeof(s->snapshot.token));
    s->ui = NULL;
    s->timer = NULL;
}

void vibe_ui_get_status(const vibe_ui_t *s, vibe_ui_status_t *out)
{
    if (!s || !out)
        return;
    *out = (vibe_ui_status_t){
        .page = s->page,
        .password_length = strlen(s->password),
        .password_visible = s->password_visible,
        .download_pending = s->download_pending,
        .rate_valid = s->rate.valid,
        .rate_bytes_per_second = s->rate.bytes_per_second,
        .grid_page = s->grid_page, .language = s->snapshot.settings.language,
        .asleep = s->page == VIBE_SLEEP, .pending_delete = s->undo_until != 0,
    };
    memcpy(out->idea_code, s->idea_code, sizeof(out->idea_code));
}

void vibe_show(vibe_ui_t *s, vibe_page_t page) { show(s, page); }
int vibe_command(vibe_ui_t *s, vibe_command_t cmd, const char *a, const char *b) { return command(s, cmd, a, b); }
void vibe_error(vibe_ui_t *s, const char *title, int error) { error_result(s, title, error); }
