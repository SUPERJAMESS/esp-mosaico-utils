// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_ids.h"
#include "vibe_ui.h"
#include "ideas_presentation.h"
#include "vibe_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static vibe_ui_t *state;
static vibe_snapshot_t model;
static unsigned connect_ticks;
static unsigned connect_attempts, bridge_open_calls;
static unsigned bridge_prefetch_calls;
static unsigned prepare_calls, cancel_calls, preview_ticks;
static unsigned download_calls, install_calls, check_calls, restart_calls;
static unsigned save_calls, delete_calls, reset_calls, next_key, scan_calls, cancel_ticks;
static unsigned scan_ticks;
static vibe_command_t current_job;
static char last_code[7];
static bool bridge_active;
static bool connecting, installing;
static bool renewed_code;
static void *trace_timer;
static char scenario_name[64], state_path[1024], control_path[1024];
static void read_environment(const char *name, char *buffer, size_t size)
{
#ifdef _WIN32
    /* Python and a MinGW DLL can have different CRT environment caches.
     * Read the process environment that the Python launcher updates. */
    DWORD length = GetEnvironmentVariableA(name, buffer, (DWORD)size);
    if (!length || length >= size) buffer[0] = 0;
#else
    const char *value = getenv(name);
    if (snprintf(buffer, size, "%s", value ? value : "") >= (int)size)
        buffer[0] = 0;
#endif
}
static bool scenario(const char *name) {
    return !strcmp(scenario_name, name);
}
static void trace_state(vui_handle_t ui, void *ctx)
{
    (void)ctx;
    const char *control = control_path;
    FILE *request = control[0] ? fopen(control, "r") : NULL;
    if (request) {
        char command[256] = {0};
        if (!fgets(command, sizeof(command), request)) command[0] = 0;
        fclose(request);
        remove(control);
        if (!strcmp(command, "release-available") || !strncmp(command, "release-available ", 18)) {
            model.release_available = model.release_installable = true;
            snprintf(model.release_version, sizeof(model.release_version), "0.2.1");
            (void)sscanf(command, "release-available %31s", model.release_version);
        }
        if (!strcmp(command, "ideas-refresh-busy")) model.idea_busy = true;
        if (!strcmp(command, "firmware-fail")) {
            model.updating = false;
            model.update_terminal = model.update_failed = true;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "Firmware update failed");
            snprintf(model.update_detail, sizeof(model.update_detail), "Error 0x00000105");
        }
        if (!strcmp(command, "firmware-complete")) {
            model.progress = 1000;
            snprintf(model.update_detail, sizeof(model.update_detail), "Restarting Vibe Mode");
        }
        if (!strcmp(command, "ideas-fail")) {
            installing = model.updating = model.transfer.receiving = false;
            model.update_terminal = model.update_failed = true;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "Download failed");
            snprintf(model.update_detail, sizeof(model.update_detail), "File integrity check failed");
            vibe_ui_poll(state);
        }
        if (!strcmp(command, "open-bridge"))
            (void)vibe_ui_open_bridge(state);
        if (!strcmp(command, "expire-code"))
            renewed_code = true;
        unsigned phase, verified, files;
        unsigned long long checked, expected, clock;
        if (sscanf(command, "open-read %llu %llu %u %u %llu", &checked, &expected,
                   &verified, &files, &clock) == 5) {
            installing = false;
            model.transfer = (vibe_transfer_t){.source = 3, .phase = VIBE_TRANSFER_READ_FILES,
                .receiving = true, .received = checked, .total = expected,
                .completed_components = verified, .component_count = files, .now_ms = clock};
            model.transfer.job[0] = 43;
            model.progress = vibe_transfer_permille(checked, expected);
            model.updating = model.cancellable = true;
            model.update_terminal = model.update_failed = model.update_cancelled = false;
            snprintf(model.update_title, sizeof(model.update_title), "Setting boot application");
            snprintf(model.update_detail, sizeof(model.update_detail), "Reading and verifying files");
            snprintf(model.update_owner, sizeof(model.update_owner), "NAND");
            vibe_ui_poll(state);
        }
        if (!strcmp(command, "library-refreshed")) {
            ++model.nand_generation;
            model.update_terminal = model.update_cancelled = false;
            vibe_ui_poll(state);
        }
        if (sscanf(command, "ideas-phase %u %llu %llu %u %u %llu", &phase, &checked,
                   &expected, &verified, &files, &clock) == 6) {
            installing = false;
            model.transfer = (vibe_transfer_t){.source = 3, .phase = phase < 4 ? phase : 3,
                .receiving = phase == 0, .received = checked, .total = expected,
                .completed_components = verified, .component_count = files, .now_ms = clock};
            if (phase == 2) model.transfer.received = model.transfer.total = 0;
            model.transfer.job[0] = 42;
            model.progress = vibe_transfer_permille(checked, expected);
            model.updating = phase < 4;
            model.update_terminal = phase == 4;
            model.update_failed = false;
            ++model.update_revision;
            const char *titles[] = {"Downloading Idea", "Verifying files", "Checking image",
                                    "Saving Idea", "Idea saved"};
            snprintf(model.update_title, sizeof(model.update_title), "%s", titles[phase < 5 ? phase : 4]);
            snprintf(model.update_detail, sizeof(model.update_detail), "%s",
                     phase == 0 ? "Transferring files" : phase == 4 ?
                     "Saved in My Ideas.\nSelect it to set as boot app." : "Please wait");
            snprintf(model.update_owner, sizeof(model.update_owner), "Cloud");
            snprintf(model.update_verified, sizeof(model.update_verified), "SHA-256");
            vibe_ui_poll(state);
        }
        if (!strcmp(command, "ai")) next_key = 1;
        if (!strcmp(command, "boot")) next_key = 3;
        if (!strcmp(command, "motion")) ++model.device.motion_revision;
        unsigned battery;
        if (sscanf(command, "battery %u", &battery) == 1) model.device.battery_percent = battery;
        char firmware[sizeof(model.firmware_version)];
        if (sscanf(command, "firmware %32s", firmware) == 1) {
            snprintf(model.firmware_version, sizeof(model.firmware_version), "%s", firmware);
            vibe_ui_poll(state);
        }
        unsigned usb_connected, usb_owner;
        if (sscanf(command, "usb %u %u", &usb_connected, &usb_owner) == 2) {
            model.device.usb_connected = usb_connected != 0;
            model.usb_owner = usb_owner != 0;
            vibe_ui_poll(state);
        }
        unsigned long long ms, received, total;
        unsigned component, count, completed, receiving, job, component_received, component_size,
            terminal;
        if (sscanf(command, "transfer %llu %llu %llu %u %u %u %u %u %u %u %u", &ms, &received,
                   &total, &component, &count, &completed, &receiving, &job, &component_received,
                   &component_size, &terminal) == 11) {
            model.transfer = (vibe_transfer_t){.now_ms = ms,
                                               .received = received,
                                               .total = total,
                                               .component_id = component,
                                               .component_count = count,
                                               .completed_components = completed,
                                               .receiving = receiving != 0,
                                               .component_received = component_received,
                                               .component_size = component_size,
                                               .source = 2};
            model.transfer.job[0] = (uint8_t)job;
            model.progress = vibe_transfer_permille(received, total);
            model.updating = terminal == 0;
            model.update_terminal = terminal != 0;
            model.update_failed = terminal == 2;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "%s",
                     terminal == 2 ? "Update failed"
                     : terminal    ? "Update complete"
                                   : "Updating system");
            snprintf(model.update_detail, sizeof(model.update_detail), "%s",
                     terminal == 2        ? "Connection interrupted - retry"
                     : terminal           ? "System images verified and committed"
                     : receiving          ? "Downloading system component"
                     : completed == count ? "Committing protected system images"
                                          : "Component verified");
            snprintf(model.update_owner, sizeof(model.update_owner), "Bridge");
            snprintf(model.update_verified, sizeof(model.update_verified), "Unsigned");
            vibe_ui_poll(state);
        }
    }
    const char *path = state_path;
    if (!path[0])
        return;
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return;
    FILE *file = fopen(temporary, "w");
    if (!file)
        return;
    vibe_ui_status_t status = {0};
    vibe_ui_get_status(state, &status);
    /* Observable test state never contains credentials or pairing tokens. */
    fprintf(file,
            "{\"page\":%d,\"password_length\":%u,\"password_visible\":%s,\"pending\":%s,\"bridge_"
            "running\":%s,\"network\":%d,\"progress\":%u,\"bridge_open_calls\":%u,\"bridge_"
            "prefetch_calls\":%u,\"bridge_active\":%s,\"code_ready\":%s,\"code_length\":%u,"
            "\"renewed_code\":%s,\"rate_valid\":%s,\"rate_bps\":%llu,\"component_id\":%u,\"sample_"
            "ms\":%llu,\"idea_code\":\"%s\",\"last_code\":\"%s\",\"download_calls\":%u,\"install_"
            "calls\":%u,\"check_calls\":%u,\"restart_calls\":%u,\"prepare_calls\":%u,\"cancel_"
            "calls\":%u,\"idea_ready\":%s,\"idea_loading\":%s,\"boot_first\":%s,",
            status.page, (unsigned)status.password_length,
            status.password_visible ? "true" : "false", status.download_pending ? "true" : "false",
            model.bridge_running ? "true" : "false", model.network, model.progress,
            bridge_open_calls, bridge_prefetch_calls, bridge_active ? "true" : "false",
            model.bridge_code[0] ? "true" : "false", (unsigned)strlen(model.bridge_code),
            renewed_code ? "true" : "false", status.rate_valid ? "true" : "false",
            (unsigned long long)status.rate_bytes_per_second, model.transfer.component_id,
            (unsigned long long)model.transfer.now_ms, status.idea_code, last_code, download_calls,
            install_calls, check_calls, restart_calls, prepare_calls, cancel_calls,
            model.idea_ready ? "true" : "false", model.idea_loading ? "true" : "false",
            model.bundles[0].boot ? "true" : "false");
    fprintf(file, "\"settings_offset\":%d,\"display_offset\":%d,",
            vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_SETTINGS_VIEWPORT),
            vui_scroll_offset(ui, VUI_VIBE_OBJ_KEY_DISPLAY_VIEWPORT));
    fprintf(file, "\"scan_calls\":%u,\"cancellable\":%s,\"cancelled\":%s,",
            scan_calls, model.cancellable ? "true" : "false", model.update_cancelled ? "true" : "false");
    fprintf(file, "\"grid_page\":%u,\"language\":%u,\"asleep\":%s,\"pending_delete\":%s,"
            "\"bundle_count\":%u,\"shared_count\":%u,\"save_calls\":%u,\"delete_calls\":%u,\"reset_calls\":%u,"
            "\"development_saved\":%s,\"brightness\":%u,\"screen_off_seconds\":%u,\"rotate\":%s,\"haptics\":%s,\"batch\":%s,\"nand_scanning\":%s}\n",
            status.grid_page, status.language, status.asleep ? "true" : "false", status.pending_delete ? "true" : "false",
            (unsigned)model.bundle_count, (unsigned)model.settings.shared_count, save_calls, delete_calls, reset_calls,
            model.development_saved ? "true" : "false", model.settings.brightness, model.settings.screen_off_seconds,
            model.settings.auto_rotate ? "true" : "false", model.settings.haptics ? "true" : "false",
            model.batch ? "true" : "false", model.nand_scanning ? "true" : "false");
    fclose(file);
#ifdef _WIN32
    /* The Windows C runtime's rename cannot replace an existing snapshot. */
    (void)MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING);
#else
    (void)rename(temporary, path);
#endif
}
static void snapshot(void *ctx, vibe_snapshot_t *out) {
    (void)ctx;
    if (cancel_ticks && !--cancel_ticks) {
        model.updating = model.idea_busy = false;
        model.update_terminal = model.update_cancelled = true;
        ++model.update_revision;
        snprintf(model.update_title, sizeof(model.update_title), "Cancelled");
        snprintf(model.update_detail, sizeof(model.update_detail), "Previously saved Ideas are unchanged");
    }
    if (model.nand_scanning && ++scan_ticks >= (scenario("scan-open") ? 80U : 30U)) {
        model.nand_scanning = model.idea_busy = false;
        if (scenario("scan-open")) {
            model.development_available = true;
            model.development_saved = false;
        }
    }
    if (connecting && ++connect_ticks >= 5) {
        connecting = false;
        model.network = VIBE_NET_CONNECTED;
        snprintf(model.ip, sizeof(model.ip), "192.0.2.10");
    }
    if (model.idea_loading && ++preview_ticks >= (scenario("slow-preview") ? 30U : 3U)) {
        model.idea_loading = model.idea_busy = false;
        model.idea_ready = true;
        if (scenario("preview-fail") ||
            (scenario("already-downloaded") && current_job == VIBE_CHECK_IDEA)) {
            model.idea_ready = false;
            model.update_terminal = true;
            model.update_failed = scenario("preview-fail");
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "%s",
                     model.update_failed ? "App information failed" : "Already up to date");
        }
    }
    if (installing) {
        model.progress += scenario("batch") ? 2 : 50;
        model.transfer.now_ms += 250;
        model.transfer.received = model.transfer.total * model.progress / 1000;
        model.transfer.component_received = (uint32_t)model.transfer.received;
        if (scenario("update-fail") && model.progress >= 500) {
            installing = false;
            model.updating = false;
            model.update_terminal = model.update_failed = true;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "Update failed");
            snprintf(model.update_detail, sizeof(model.update_detail),
                     "Simulated verification error");
        } else if (model.progress >= 1000) {
            installing = false;
            model.updating = false;
            model.update_terminal = true;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "%s",
                     (current_job == VIBE_INSTALL_NAND || current_job == VIBE_OPEN_IDEA) ? "Boot application ready"
                                                      : "Download complete");
            snprintf(model.update_detail, sizeof(model.update_detail), "%s",
                     (current_job == VIBE_INSTALL_NAND || current_job == VIBE_OPEN_IDEA) ? "Starts on next power-on"
                                                      : "Saved in My Ideas");
            if ((current_job == VIBE_INSTALL_NAND || current_job == VIBE_OPEN_IDEA))
                for (size_t i = 0; i < model.bundle_count; i++)
                    model.bundles[i].boot = !strcmp(model.bundles[i].code, last_code);
            if (current_job == VIBE_OPEN_IDEA) ++restart_calls;
            if (current_job == VIBE_SAVE_DEVELOPMENT) model.development_saved = true;
            model.cancellable = model.batch = model.idea_busy = false;
            ++model.nand_generation;
        }
        model.transfer.receiving = installing;
    }
    if (model.bridge_running && model.network == VIBE_NET_CONNECTED && !scenario("waiting-code")) {
        snprintf(model.bridge_state, sizeof(model.bridge_state), "PAIRING");
        snprintf(model.bridge_code, sizeof(model.bridge_code), "%s",
                 scenario("long-code") ? "WWWWWWWWWWWWWWW"
                 : renewed_code        ? "NEW1234567"
                                       : "VIBE123456");
        model.bridge_seconds = 300;
    }
    *out = model;
}
static int command(void *ctx, vibe_command_t cmd, const char *a, const char *b) {
    (void)ctx;
    switch (cmd) {
    case VIBE_CHECK_FIRMWARE:
        ++model.release_generation;
        model.release_error = 0;
        break;
    case VIBE_INSTALL_FIRMWARE:
        if (!model.release_installable || !a || strcmp(a, model.release_version)) return 0x106;
        model.firmware_update = model.updating = true;
        model.update_terminal = model.update_failed = model.cancellable = model.batch = false;
        model.progress = 250;
        ++model.update_revision;
        snprintf(model.update_title, sizeof(model.update_title), "Updating firmware");
        snprintf(model.update_detail, sizeof(model.update_detail), "Downloading firmware");
        break;
    case VIBE_SET_LANGUAGE:
        model.settings.language = !strcmp(a, "en") ? 1 : 2;
        ++model.nand_generation;
        break;
    case VIBE_SCAN_WIFI:
        ++model.scan_generation;
        break;
    case VIBE_FORGET_WIFI:
        model.credentials_saved = false;
        model.network = VIBE_NET_OFFLINE;
        model.ip[0] = 0;
        connecting = false;
        break;
    case VIBE_CONNECT_WIFI:
        if (strlen(b) != 0 && strlen(b) < 8)
            return 0x102;
        snprintf(model.ssid, sizeof(model.ssid), "%s", a);
        ++connect_attempts;
        if (!strcmp(b, "wrongpass") || (scenario("wifi-fail-once") && connect_attempts == 1)) {
            model.network = VIBE_NET_FAILED;
            break;
        }
        model.network = VIBE_NET_CONNECTING;
        model.credentials_saved = true;
        connecting = true;
        connect_ticks = 0;
        break;
    case VIBE_OPEN_BRIDGE:
        ++bridge_open_calls;
        bridge_active = true;
        if (model.bridge_running)
            break;
        model.bridge_running = true;
        snprintf(model.bridge_state, sizeof(model.bridge_state), "WAITING_NETWORK");
        break;
    case VIBE_PREFETCH_BRIDGE:
        ++bridge_prefetch_calls;
        model.bridge_running = true;
        break;
    case VIBE_PAUSE_BRIDGE:
        bridge_active = false;
        break;
    case VIBE_STOP_BRIDGE:
        model.bridge_running = false;
        bridge_active = false;
        model.bridge_code[0] = 0;
        break;
    case VIBE_DELETE_IDEA:
        for (size_t i = 0; i < model.bundle_count; ++i) {
            if (strcmp(model.bundles[i].code, a)) continue;
            if (model.bundles[i].boot) return 0x103;
            memmove(&model.bundles[i], &model.bundles[i + 1], (--model.bundle_count - i) * sizeof(model.bundles[0]));
            ++delete_calls;
            ++model.nand_generation;
            break;
        }
        break;
    case VIBE_SCAN_NAND:
        ++scan_calls;
        if (scenario("scan-busy") || scenario("scan-open")) {
            model.nand_scanning = model.idea_busy = true;
            scan_ticks = 0;
        }
        ++model.nand_generation;
        break;
    case VIBE_RUN_DEVELOPMENT:
    case VIBE_START_IDEA:
        ++restart_calls;
        break;
    case VIBE_CANCEL_IDEA:
        ++cancel_calls;
        if (model.cancellable) {
            if (scenario("open-check") && current_job == VIBE_OPEN_IDEA) {
                installing = model.cancellable = model.transfer.receiving = false;
                cancel_ticks = 8;
                snprintf(model.update_title, sizeof(model.update_title), "Cancelling...");
                snprintf(model.update_detail, sizeof(model.update_detail), "Please wait");
                break;
            }
            installing = model.updating = model.cancellable = model.batch = false;
            model.update_terminal = model.update_cancelled = true;
            ++model.update_revision;
            snprintf(model.update_title, sizeof(model.update_title), "Cancelled");
        }
        model.idea_loading = model.idea_ready = model.idea_busy = false;
        break;
    case VIBE_PREVIEW_IDEA:
    case VIBE_CHECK_IDEA:
    case VIBE_RESOLVE_IDEA:
        if (model.idea_busy || model.idea_ready)
            return 0x103;
        current_job = cmd;
        if (cmd == VIBE_CHECK_IDEA)
            ++check_calls;
        ++prepare_calls;
        preview_ticks = 0;
        snprintf(last_code, sizeof(last_code), "%s", a);
        model.idea_loading = model.idea_busy = true;
        model.idea_ready = false;
        model.idea_custom = scenario("custom-preview") || scenario("custom-preview-fail");
        model.idea_view_only = cmd == VIBE_PREVIEW_IDEA;
        model.idea_return_library = false;
        model.update_terminal = false;
        ++model.idea_request;
        model.idea = model.bundles[0];
        snprintf(model.idea.code, sizeof(model.idea.code), "%s", a);
        snprintf(model.idea.version, sizeof(model.idea.version), "1.1");
        model.idea_downloaded = scenario("already-downloaded");
        break;
    case VIBE_DOWNLOAD_IDEA:
        if (model.idea_view_only || !model.idea_ready || !b ||
            strtoul(b, NULL, 10) != model.idea_request)
            return 0x103;
        model.idea_ready = false;
        if (model.idea_downloaded) {
            model.idea_return_library = true;
            break;
        }
        /* fall through */
    case VIBE_SAVE_DEVELOPMENT:
    case VIBE_UPDATE_ALL:
    case VIBE_OPEN_IDEA:
    case VIBE_INSTALL_NAND:
        current_job = cmd;
        if (cmd == VIBE_DOWNLOAD_IDEA)
            ++download_calls;
        else if (cmd == VIBE_SAVE_DEVELOPMENT) ++save_calls;
        else if (cmd == VIBE_UPDATE_ALL) ++check_calls;
        else ++install_calls;
        model.cancellable = cmd == VIBE_DOWNLOAD_IDEA || cmd == VIBE_UPDATE_ALL;
        model.batch = cmd == VIBE_UPDATE_ALL;
        model.batch_total = (unsigned)model.bundle_count;
        model.batch_index = 1;
        if (model.batch && model.bundle_count) {
            snprintf(model.batch_code, sizeof(model.batch_code), "%s", model.bundles[0].code);
            for (size_t i = 0; i < model.bundle_count; ++i) model.bundles[i].queued = true;
        }
        snprintf(last_code, sizeof(last_code), "%s", a ? a : "");
        installing = true;
        model.updating = true;
        model.update_terminal = false;
        model.progress = 0;
        model.update_failed = false;
        model.update_cancelled = false;
        ++model.update_revision;
        model.transfer = (vibe_transfer_t){.source = 3,
                                           .total = 1024 * 1024,
                                           .component_size = 1024 * 1024,
                                           .component_count = 1,
                                           .component_id = 1,
                                           .receiving = true};
        model.transfer.job[0] = (uint8_t)model.update_revision;
        snprintf(model.update_title, sizeof(model.update_title), "Updating system");
        snprintf(model.update_detail, sizeof(model.update_detail),
                 "Reading simulated NAND component");
        snprintf(model.update_owner, sizeof(model.update_owner), "NAND");
        snprintf(model.update_verified, sizeof(model.update_verified), "Unsigned");
        if (cmd == VIBE_OPEN_IDEA && scenario("open-check")) {
            installing = false;
            model.cancellable = model.idea_busy = true;
            model.transfer.phase = VIBE_TRANSFER_READ_FILES;
            snprintf(model.update_title, sizeof(model.update_title), "Setting boot application");
            snprintf(model.update_detail, sizeof(model.update_detail), "Reading and verifying files");
        }
        break;
    }
    vibe_ui_status_t status = {0};
    vibe_ui_get_status(state, &status);
    fprintf(stderr, "vibe_sim: command=%d page=%d\n", cmd, status.page);
    return 0;
}
/* Synthetic pixels exercise the production adapter without font/UI coupling.
 * The portable font reader has separate byte-level publication roundtrips. */
static void *preview(void *ctx, uint32_t request, unsigned slot, size_t *size) {
    (void)ctx;
    *size = 0;
    if (request != model.idea_request || slot > 1 || scenario("custom-preview-fail"))
        return NULL;
    size_t count = slot ? IDEAS_DESCRIPTION_WIDTH * IDEAS_DESCRIPTION_HEIGHT
                        : IDEAS_TITLE_WIDTH * IDEAS_TITLE_HEIGHT;
    uint16_t *pixels = malloc(count * 2);
    if (pixels) {
        for (size_t i = 0; i < count; ++i)
            pixels[i] = slot ? 0x07e0 : 0xf800;
        *size = count * 2;
    }
    return pixels;
}
static int setting(void *ctx, vibe_settings_command_t cmd, const char *key, const char *value)
{
    (void)ctx;
    unsigned n = value ? (unsigned)strtoul(value, NULL, 10) : 0;
    switch (cmd) {
    case VIBE_SETTINGS_LANGUAGE: model.settings.language = n; break;
    case VIBE_SETTINGS_BRIGHTNESS: model.settings.brightness = n; break;
    case VIBE_SETTINGS_TIMEOUT: model.settings.screen_off_seconds = n; break;
    case VIBE_SETTINGS_ROTATE: model.settings.auto_rotate = n; break;
    case VIBE_SETTINGS_HAPTICS: model.settings.haptics = n; break;
    case VIBE_SETTINGS_OPEN_CONFIRM: model.settings.open_confirm = n; break;
    case VIBE_SETTINGS_SHARED_SET:
    case VIBE_SETTINGS_SHARED_DELETE: {
        size_t i;
        for (i = 0; i < model.settings.shared_count && strcmp(model.settings.shared[i].key, key); ++i) {}
        if (cmd == VIBE_SETTINGS_SHARED_DELETE) {
            if (i < model.settings.shared_count)
                memmove(&model.settings.shared[i], &model.settings.shared[i + 1],
                        (--model.settings.shared_count - i) * sizeof(model.settings.shared[0]));
        } else {
            if (i >= VIBE_SHARED_MAX) return 0x101;
            if (i == model.settings.shared_count) ++model.settings.shared_count;
            snprintf(model.settings.shared[i].key, sizeof(model.settings.shared[i].key), "%s", key);
            snprintf(model.settings.shared[i].value, sizeof(model.settings.shared[i].value), "%s", value);
        }
        break;
    }
    case VIBE_SETTINGS_RESET:
        ++reset_calls;
        ++restart_calls;
        model.settings.language = 1;
        model.settings.shared_count = 0;
        memset(model.settings.shared, 0, sizeof(model.settings.shared));
        model.credentials_saved = false;
        model.network = VIBE_NET_OFFLINE;
        break;
    }
    ++model.settings.revision;
    return 0;
}
static unsigned key(void *ctx) { (void)ctx; unsigned k = next_key; next_key = 0; return k; }
vui_err_t vibe_pc_init(vui_handle_t ui)
{
    read_environment("VIBE_SIM_SCENARIO", scenario_name, sizeof(scenario_name));
    read_environment("VIBE_SIM_STATE_FILE", state_path, sizeof(state_path));
    read_environment("VIBE_SIM_CONTROL_FILE", control_path, sizeof(control_path));
    model.settings = (vibe_settings_snapshot_t){.language = 1,
        .brightness = 80, .screen_off_seconds = 180, .auto_rotate = true, .haptics = true, .open_confirm = true, .revision = 1};
    model.device.battery_valid = true;
    model.device.battery_percent = scenario("low-battery") ? 15 : 80;
    model.development_available = scenario("unsaved") || scenario("development");
    snprintf(model.development_name, sizeof(model.development_name), "My development Idea");
    snprintf(model.development_version, sizeof(model.development_version), "1.0");
    snprintf(model.firmware_version, sizeof(model.firmware_version), "0.3-dev");
    snprintf(model.mac, sizeof(model.mac), "02:00:00:00:00:01");
    model.storage_total = 512ULL * 1048576;
    model.storage_used = 2ULL * 1048576;
    model.tcp_port = 7777;
    if (scenario("prefetched") || scenario("long-code") || scenario("waiting-code")) {
        model.credentials_saved = true;
        model.network = VIBE_NET_CONNECTED;
        snprintf(model.ip, sizeof(model.ip), "192.0.2.10");
    }
    snprintf(model.hostname, sizeof(model.hostname), "mosaico-simulator");
    model.ap_count = 6;
    model.scan_generation = 1;
    if (scenario("empty"))
        model.ap_count = 0;
    for (size_t i = 0; i < model.ap_count; ++i) {
        snprintf(model.aps[i].ssid, sizeof(model.aps[i].ssid), "Studio network %u",
                 (unsigned)i + 1);
        model.aps[i].rssi = -35 - (int)i * 7;
        model.aps[i].open = i == 5;
    }
    memset(model.token, 'a', 64);
    model.token[64] = 0;
    model.bundle_count = 2;
    model.nand_generation = 1;
    model.nand_scanning = model.idea_busy = scenario("scan-busy") || scenario("scan-open");
    if (scenario("empty"))
        model.bundle_count = 0;
    if (scenario("pager-five")) model.bundle_count = 5;
    if (scenario("pager-six") || scenario("manage-states")) model.bundle_count = 6;
    if (scenario("pager-max")) model.bundle_count = VIBE_BUNDLE_MAX;
    if (scenario("manage-states")) {
        model.storage_total = 64000000;
        model.storage_used = 32800000;
    }
    for (size_t i = 0; i < model.bundle_count; ++i) {
        snprintf(model.bundles[i].release, sizeof(model.bundles[i].release), "Demo release %u",
                 (unsigned)i + 1);
        snprintf(model.bundles[i].code, sizeof(model.bundles[i].code), "%06u", 42731 + (unsigned)i);
        snprintf(model.bundles[i].version, sizeof(model.bundles[i].version), "1.0");
        snprintf(model.bundles[i].description, sizeof(model.bundles[i].description),
                 "A downloaded Idea");
        model.bundles[i].boot = i == 1;
        if (scenario("manage-states")) {
            model.bundles[i].boot = i == 0;
            model.bundles[i].update_available = i == 1 || i == 2;
        }
        model.bundles[i].bytes = 1024 * 1024;
        model.bundles[i].components = 2;
    }
    const vibe_services_t services = {.snapshot = snapshot, .command = command, .preview = preview, .setting = setting, .key = key};
    vui_err_t err = vibe_ui_create(ui, &services, &state);
    if (err == VUI_OK && state_path[0])
        trace_timer = vui_timer_create(ui, 50, trace_state, NULL);
    return err;
}
void vibe_pc_deinit(vui_handle_t ui)
{
    (void)ui;
    if (trace_timer)
        (void)vui_timer_delete(ui, trace_timer);
    vibe_ui_deinit(state);
}
