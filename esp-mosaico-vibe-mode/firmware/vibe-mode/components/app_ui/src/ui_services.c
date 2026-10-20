#include "esp_attr.h"
// SPDX-License-Identifier: Apache-2.0
#include "esp_iris.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "factory_ideas.h"
#include "factory_locale.h"
#include "factory_nand_update.h"
#include "factory_network.h"
#include "factory_update_check.h"
#include "factory_firmware_install.h"
#include "factory_system_update.h"
#include "vibe_settings.h"
#include "vibe_device.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "iris_bridge.h"
#include "sdkconfig.h"
#include "vibe_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_services.h"
typedef struct {
    factory_nand_update_snapshot_t nand;
    factory_ideas_snapshot_t ideas;
    uint32_t ideas_revision, firmware_revision;
    esp_iris_system_update_phase_t phase;
    factory_nand_update_state_t nand_state;
    uint32_t ota_job;
    int ota_state;
    uint32_t revision;
    bool rescan_due, updates_checked, locale_refresh_due;
    unsigned language;
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    enum { UPDATE_NONE, UPDATE_OTA, UPDATE_SYSTEM, UPDATE_NAND, UPDATE_IDEAS, UPDATE_FIRMWARE } update_source;
} ui_services_context_t;
/* Large render-owned snapshots remain zero-filled PSRAM storage. No locks. */
static EXT_RAM_BSS_ATTR ui_services_context_t s_services;

static int service_command(void *ctx, vibe_command_t cmd, const char *a, const char *b) {
    (void)ctx;
    switch (cmd) {
    case VIBE_CHECK_FIRMWARE:
        factory_update_check_request(true);
        return ESP_OK;
    case VIBE_INSTALL_FIRMWARE: {
        factory_update_check_snapshot_t release = {0};
        factory_update_check_snapshot(&release);
        if (!a || strcmp(a, release.version)) return ESP_ERR_INVALID_VERSION;
        return factory_firmware_install_start(&release);
    }
    case VIBE_SET_LANGUAGE: {
        if (!a || (strcmp(a, "en") && strcmp(a, "zh-CN")))
            return ESP_ERR_INVALID_ARG;
        return vibe_settings_request(VIBE_SETTINGS_LANGUAGE, NULL, !strcmp(a, "en") ? "1" : "2");
    }
    case VIBE_SCAN_WIFI:
        return factory_network_request_scan();
    case VIBE_FORGET_WIFI:
        return factory_network_forget();
    case VIBE_CONNECT_WIFI:
        return factory_network_connect(a, b);
    case VIBE_OPEN_BRIDGE:
        return ESP_OK;
    case VIBE_PREFETCH_BRIDGE:
        return ESP_OK;
    case VIBE_PAUSE_BRIDGE:
        iris_bridge_set_active(false);
        return ESP_OK;
    case VIBE_STOP_BRIDGE:
        iris_bridge_stop();
        return ESP_OK;
    case VIBE_STORAGE_INFO:
        return factory_ideas_request(IDEAS_STORAGE_INFO, NULL);
    case VIBE_FORMAT_NAND:
        return factory_ideas_request(IDEAS_FORMAT_NAND, NULL);
    case VIBE_SCAN_NAND:
        return factory_ideas_request(IDEAS_SCAN, NULL);
    case VIBE_INSTALL_NAND:
        return factory_ideas_request(IDEAS_INSTALL, a);
    case VIBE_RESOLVE_IDEA:
        return factory_ideas_request(IDEAS_RESOLVE, a);
    case VIBE_CANCEL_IDEA:
        return factory_ideas_cancel();
    case VIBE_DOWNLOAD_IDEA:
        return factory_ideas_confirm(b ? strtoul(b, NULL, 10) : 0, true);
    case VIBE_CHECK_IDEA:
        return factory_ideas_request(IDEAS_CHECK, a);
    case VIBE_START_IDEA:
        return factory_ideas_request(IDEAS_START, a);
    case VIBE_PREVIEW_IDEA:
        return factory_ideas_request(IDEAS_PREVIEW, a);
    case VIBE_OPEN_IDEA:
        return factory_ideas_request(IDEAS_OPEN, a);
    case VIBE_DELETE_IDEA:
        return factory_ideas_request(IDEAS_DELETE, a);
    case VIBE_SAVE_DEVELOPMENT:
        return factory_ideas_request(IDEAS_SAVE_DEVELOPMENT, NULL);
    case VIBE_RUN_DEVELOPMENT:
        return factory_ideas_request(IDEAS_RUN_DEVELOPMENT, NULL);
    case VIBE_UPDATE_ALL:
        return factory_ideas_request(IDEAS_UPDATE_ALL, NULL);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}
static void *service_icon(void *ctx, const char *code, size_t *size) {
    (void)ctx;
    return factory_ideas_copy_icon(code, size);
}
static void *service_title(void *ctx, const char *code, size_t *size) {
    (void)ctx;
    return factory_ideas_copy_title(code, size);
}
static void *service_preview(void *ctx, uint32_t request, unsigned slot, size_t *size) {
    (void)ctx;
    return factory_ideas_copy_preview(request, slot, size);
}
#define COPY(field, value) snprintf(out->field, sizeof(out->field), "%s", value)
static void service_snapshot(void *argument, vibe_snapshot_t *out) {
    ui_services_context_t *ctx = argument;
    (void)vibe_settings_snapshot(&out->settings);
    if (ctx->language != out->settings.language) {
        ctx->language = out->settings.language;
        ctx->locale_refresh_due = true;
    }
    (void)vibe_device_snapshot(&out->device);
    snprintf(out->firmware_version, sizeof(out->firmware_version), "%s", esp_app_get_description()->version);
    if (!out->mac[0]) {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
            snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    factory_network_snapshot_t network = {0};
    esp_iris_status_t iris = {0};
    iris_bridge_snapshot_t bridge = {0};
    factory_update_check_snapshot_t release = {0};
    factory_update_check_snapshot(&release);
    out->release_checking = release.checking;
    out->release_generation = release.generation;
    out->release_error = release.error;
    out->release_available = release.available;
    out->release_installable = release.installable;
    out->firmware_update = false;
    COPY(release_version, release.version);
    (void)factory_network_get_snapshot(&network);
    (void)esp_iris_get_status(&iris);
    iris_bridge_get_snapshot(&bridge);
    out->network = network.state == FACTORY_NETWORK_CONNECTED    ? VIBE_NET_CONNECTED
                   : network.state == FACTORY_NETWORK_CONNECTING ? VIBE_NET_CONNECTING
                   : network.state == FACTORY_NETWORK_FAILED     ? VIBE_NET_FAILED
                                                                 : VIBE_NET_OFFLINE;
    out->credentials_saved = network.credentials_saved;
    out->scanning = network.scanning;
    out->session = iris.session_ready;
    out->usb_owner = iris.session_ready && iris.transport == ESP_IRIS_TRANSPORT_KIND_USB;
    out->tcp_owner = iris.session_ready && iris.transport == ESP_IRIS_TRANSPORT_KIND_TCP;
    COPY(ssid, network.ssid);
    COPY(ip, network.ip);
    COPY(hostname, network.hostname);
    out->tcp_port = CONFIG_ESP_IRIS_TCP_PORT;
    out->token[0] = '\0';
    (void)esp_iris_pairing_token_get(out->token);
    out->scan_generation = network.scan_generation;
    out->ap_count = network.ap_count < VIBE_AP_MAX ? network.ap_count : VIBE_AP_MAX;
    for (size_t i = 0; i < out->ap_count; ++i) {
        snprintf(out->aps[i].ssid, sizeof(out->aps[i].ssid), "%s", network.aps[i].ssid);
        out->aps[i].rssi = network.aps[i].rssi;
        out->aps[i].open = network.aps[i].authmode == WIFI_AUTH_OPEN;
    }
    out->bridge_running = bridge.running;
    COPY(bridge_code, bridge.code);
    COPY(bridge_state, bridge.state);
    out->bridge_seconds = bridge.expires_in_ms / 1000;
    out->bridge_error = bridge.error;
    (void)factory_nand_update_get_snapshot(&ctx->nand);
    (void)factory_ideas_snapshot(&ctx->ideas);
    out->storage_total = ctx->ideas.storage_total;
    out->storage_used = ctx->ideas.storage_used;
    out->development_available = ctx->ideas.development_available;
    out->development_saved = ctx->ideas.development_saved;
    COPY(development_name, ctx->ideas.development_name);
    COPY(development_version, ctx->ideas.development_version);
    out->cancellable = ctx->ideas.cancellable;
    out->batch = ctx->ideas.batch;
    out->batch_index = ctx->ideas.batch_index;
    out->batch_total = ctx->ideas.batch_total;
    COPY(batch_code, ctx->ideas.batch_code);
    out->nand_generation = ctx->ideas.generation;
    out->nand_scanning = ctx->ideas.scanning;
    out->nand_error = ctx->ideas.storage_error ? ctx->ideas.storage_error : ctx->ideas.scan_error;
    out->nand_needs_format = ctx->ideas.storage_error != ESP_OK;
    out->bundle_count = ctx->ideas.count < VIBE_BUNDLE_MAX ? ctx->ideas.count : VIBE_BUNDLE_MAX;
    out->invalid_bundles = ctx->ideas.invalid;
    out->idea_busy = ctx->ideas.busy;
    out->idea_loading = ctx->ideas.resolving;
    out->idea_ready = ctx->ideas.preview_ready;
    out->idea_view_only = ctx->ideas.view_only;
    out->idea_custom = ctx->ideas.custom_display;
    out->idea_has_icon = ctx->ideas.has_icon;
    out->idea_downloaded = ctx->ideas.already_downloaded;
    out->idea_return_library = ctx->ideas.return_library;
    out->idea_request = ctx->ideas.preview_id;
    snprintf(out->idea.code, sizeof(out->idea.code), "%s", ctx->ideas.preview.code);
    snprintf(out->idea.release, sizeof(out->idea.release), "%s", ctx->ideas.preview.name);
    snprintf(out->idea.version, sizeof(out->idea.version), "%s", ctx->ideas.preview.version);
    snprintf(out->idea.description, sizeof(out->idea.description), "%s",
             ctx->ideas.preview.description);
    out->idea.bytes = ctx->ideas.preview.bytes;
    for (size_t i = 0; i < out->bundle_count; ++i) {
        const factory_idea_t *c = &ctx->ideas.items[i];
        snprintf(out->bundles[i].release, sizeof(out->bundles[i].release), "%s", c->name);
        snprintf(out->bundles[i].path, sizeof(out->bundles[i].path), "%s", c->code);
        snprintf(out->bundles[i].code, sizeof(out->bundles[i].code), "%s", c->code);
        snprintf(out->bundles[i].version, sizeof(out->bundles[i].version), "%s", c->version);
        snprintf(out->bundles[i].description, sizeof(out->bundles[i].description), "%s",
                 c->description);
        out->bundles[i].boot = c->boot;
        out->bundles[i].update_available = c->update_available;
        out->bundles[i].queued = c->queued;
        out->bundles[i].bytes = c->bytes;
        out->bundles[i].components = c->components;
    }
    esp_iris_ota_status_t ota = {0};
    factory_system_update_status_t system = {0};
    (void)esp_iris_ota_get_status(&ota);
    (void)factory_system_update_get_status(&system);
    const bool nand_changed = ctx->nand_state != ctx->nand.update_state;
    const bool ota_changed = ota.job_id != ctx->ota_job || (int)ota.state != ctx->ota_state;
    if (ota_changed && ota.job_id && !ota.active) ctx->rescan_due = true;
    if (ctx->rescan_due && !ctx->ideas.busy && factory_ideas_request(IDEAS_SCAN, NULL) == ESP_OK)
        ctx->rescan_due = false;
    /* Before first library discovery there are no cached localized rows.
     * Choosing the welcome language must not start a boot-time NAND scan. */
    if (ctx->locale_refresh_due && !ctx->ideas.busy && !ctx->ideas.generation)
        ctx->locale_refresh_due = false;
    if (ctx->locale_refresh_due && !ctx->ideas.busy && !ctx->ideas.preview_ready &&
        factory_ideas_request(IDEAS_RELOCALIZE, NULL) == ESP_OK)
        ctx->locale_refresh_due = false;
    if (!ctx->updates_checked && network.state == FACTORY_NETWORK_CONNECTED &&
        !ctx->ideas.busy && !ctx->ideas.preview_ready && ctx->ideas.generation &&
        !ota.active && !factory_system_update_writer_busy() &&
        factory_ideas_request(IDEAS_REFRESH_UPDATES, NULL) == ESP_OK)
        ctx->updates_checked = true;
    const bool system_changed =
        ctx->phase != system.update.phase ||
        memcmp(ctx->operation_id, system.update.operation_id, sizeof(ctx->operation_id)) != 0;
    if (nand_changed || ota_changed || system_changed)
        ++ctx->revision;
    ctx->nand_state = ctx->nand.update_state;
    ctx->ota_job = ota.job_id;
    ctx->ota_state = ota.state;
    ctx->phase = system.update.phase;
    memcpy(ctx->operation_id, system.update.operation_id, sizeof(ctx->operation_id));
    if (system_changed && system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED) ctx->rescan_due = true;
    const bool system_active = system.update.phase != ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE &&
                               system.update.phase < ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED;
    if (nand_changed && ctx->nand.update_state == FACTORY_NAND_UPDATE_FAILED)
        ctx->update_source = UPDATE_NAND;
    if (ota_changed && ota.job_id)
        ctx->update_source = UPDATE_OTA;
    if (system_changed && system.update.phase != ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE)
        ctx->update_source = UPDATE_SYSTEM;
    /* Active writers supersede retained failures from all other sources. */
    if (ctx->nand.update_state == FACTORY_NAND_UPDATE_STARTING)
        ctx->update_source = UPDATE_NAND;
    if (ota.active)
        ctx->update_source = UPDATE_OTA;
    if (system_active)
        ctx->update_source = UPDATE_SYSTEM;
    if (ctx->ideas.revision != ctx->ideas_revision && ctx->ideas.command != IDEAS_REFRESH_UPDATES) {
        ctx->ideas_revision = ctx->ideas.revision;
        ++ctx->revision;
        ctx->update_source = UPDATE_IDEAS;
    }
    if (ctx->ideas.busy &&
        (ctx->ideas.command == IDEAS_DOWNLOAD || ctx->ideas.command == IDEAS_INSTALL ||
         ctx->ideas.command == IDEAS_OPEN || ctx->ideas.command == IDEAS_SAVE_DEVELOPMENT || ctx->ideas.command == IDEAS_FORMAT_NAND ||
         (ctx->ideas.command == IDEAS_UPDATE_ALL || ctx->ideas.command == IDEAS_RUN_DEVELOPMENT)))
        ctx->update_source = UPDATE_IDEAS;
    if (ota.active) ctx->update_source = UPDATE_OTA;
    if (system_active && !(ctx->ideas.busy &&
        (ctx->ideas.command == IDEAS_OPEN || ctx->ideas.command == IDEAS_INSTALL)))
        ctx->update_source = UPDATE_SYSTEM;
    if (ctx->update_source != UPDATE_IDEAS) out->batch = out->cancellable = false;
    out->updating = out->update_terminal = out->update_failed = out->update_cancelled = false;
    out->progress = 0;
    memset(&out->transfer, 0, sizeof(out->transfer));
    out->transfer.now_ms = (uint64_t)esp_timer_get_time() / 1000;
    out->transfer.source = (uint8_t)ctx->update_source;
    out->update_revision = ctx->revision;
    factory_firmware_status_t firmware = {0};
    factory_firmware_install_status(&firmware);
    if (ctx->firmware_revision != firmware.revision) {
        ctx->firmware_revision = firmware.revision;
        ctx->update_source = UPDATE_FIRMWARE;
        ++ctx->revision;
    }
    if (firmware.phase != FIRMWARE_IDLE && (firmware.phase != FIRMWARE_FAILED || ctx->update_source == UPDATE_FIRMWARE)) {
        out->firmware_update = true;
        out->updating = firmware.phase != FIRMWARE_FAILED;
        out->update_terminal = out->update_failed = firmware.phase == FIRMWARE_FAILED;
        out->batch = out->cancellable = false;
        out->update_revision = ctx->revision;
        out->progress = vibe_transfer_permille(firmware.received, firmware.total);
        if (firmware.phase == FIRMWARE_WRITING || (out->update_failed && system.owner == FACTORY_SYSTEM_UPDATE_OWNER_FIRMWARE && system.commit_total)) {
            out->progress = vibe_transfer_permille(system.commit_written, system.commit_total);
            if (out->progress >= 1000) out->progress = 999;
        }
        COPY(update_title, out->update_failed ? "Firmware update failed" : "Updating firmware");
        const char *detail = firmware.phase == FIRMWARE_PREPARING ? "Preparing update" :
            firmware.phase == FIRMWARE_DOWNLOADING ? "Downloading firmware" :
            firmware.phase == FIRMWARE_VERIFYING ? "Verifying firmware" :
            firmware.phase == FIRMWARE_WRITING ? "Writing firmware" : "Restarting Vibe Mode";
        if (out->update_failed) snprintf(out->update_detail, sizeof(out->update_detail), "Error 0x%08x", (unsigned)firmware.error);
        else COPY(update_detail, detail);
        return;
    }
    if (ctx->update_source == UPDATE_IDEAS) {
        out->updating = ctx->ideas.busy && (ctx->ideas.command == IDEAS_DOWNLOAD ||
                                            ctx->ideas.command == IDEAS_INSTALL ||
                                            ctx->ideas.command == IDEAS_OPEN ||
                                            ctx->ideas.command == IDEAS_SAVE_DEVELOPMENT || ctx->ideas.command == IDEAS_FORMAT_NAND ||
                                            (ctx->ideas.command == IDEAS_UPDATE_ALL || ctx->ideas.command == IDEAS_RUN_DEVELOPMENT));
        out->update_terminal = ctx->ideas.terminal;
        if ((ctx->ideas.terminal_command == IDEAS_DELETE ||
             ctx->ideas.terminal_command == IDEAS_REFRESH_UPDATES) && !ctx->ideas.failed)
            out->update_terminal = false;
        out->update_failed = ctx->ideas.failed;
        out->update_cancelled = ctx->ideas.cancelled;
        COPY(update_title, ctx->ideas.title);
        COPY(update_detail, ctx->ideas.detail);
        COPY(update_owner, ctx->ideas.command == IDEAS_FORMAT_NAND || ctx->ideas.command == IDEAS_INSTALL || ctx->ideas.command == IDEAS_OPEN
                               ? "NAND" : ctx->ideas.command == IDEAS_SAVE_DEVELOPMENT ? "Device" : "Cloud");
        COPY(update_verified, ctx->ideas.command == IDEAS_FORMAT_NAND ? "" : "SHA-256");
        out->transfer.total = ctx->ideas.total;
        out->transfer.received = ctx->ideas.received;
        memcpy(out->transfer.job, &ctx->ideas.revision, sizeof(ctx->ideas.revision));
        out->progress = vibe_transfer_permille(ctx->ideas.received, ctx->ideas.total);
        out->transfer.receiving = out->updating && ctx->ideas.command == IDEAS_DOWNLOAD &&
                                  ctx->ideas.phase == IDEAS_PHASE_DOWNLOAD;
        if (ctx->ideas.command == IDEAS_FORMAT_NAND)
            out->transfer.phase = VIBE_TRANSFER_SAVE;
        if (ctx->ideas.command == IDEAS_DOWNLOAD) {
            if (ctx->ideas.phase == IDEAS_PHASE_VERIFY_FILES)
                out->transfer.phase = VIBE_TRANSFER_VERIFY_FILES;
            else if (ctx->ideas.phase == IDEAS_PHASE_CHECK_IMAGE) {
                out->transfer.phase = VIBE_TRANSFER_CHECK_IMAGE;
                out->transfer.received = out->transfer.total = 0;
            }
            else if (ctx->ideas.phase == IDEAS_PHASE_SAVE)
                out->transfer.phase = VIBE_TRANSFER_SAVE;
            out->transfer.completed_components = ctx->ideas.files_verified;
            out->transfer.component_count = ctx->ideas.file_count;
        }
        if (ctx->ideas.command == IDEAS_INSTALL || ctx->ideas.command == IDEAS_OPEN) {
            out->transfer.completed_components = ctx->ideas.files_verified;
            out->transfer.component_count = ctx->ideas.file_count;
            if (ctx->ideas.phase == IDEAS_PHASE_VERIFY_FILES) {
                out->transfer.phase = VIBE_TRANSFER_READ_FILES;
                out->transfer.receiving = out->updating && ctx->ideas.cancellable;
                out->transfer.completed_components = ctx->ideas.files_verified;
                out->transfer.component_count = ctx->ideas.file_count;
            } else if (ctx->ideas.phase == IDEAS_PHASE_CHECK_IMAGE)
                out->transfer.phase = VIBE_TRANSFER_CHECK_IMAGE;
            else if (ctx->ideas.phase == IDEAS_PHASE_SAVE)
                out->transfer.phase = VIBE_TRANSFER_SAVE;
        }
        if ((ctx->ideas.command == IDEAS_INSTALL || ctx->ideas.command == IDEAS_OPEN) && system_active) {
            out->transfer.phase = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING
                                      ? VIBE_TRANSFER_SAVE : VIBE_TRANSFER_BYTES;
            out->transfer.total = system.total_size;
            out->transfer.received = system.received_size;
            out->transfer.component_id = system.update.active_component_id;
            out->transfer.component_count = system.update.component_count;
            out->transfer.completed_components = system.update.completed_components;
            out->transfer.component_size = system.update.component_size;
            out->transfer.component_received = system.update.component_received;
            out->transfer.receiving = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING;
            COPY(update_detail, system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING
                                    ? "Selecting boot application"
                                    : "Writing application");
        }
        return;
    }
    if (ctx->update_source == UPDATE_NAND) {
        out->updating = ctx->nand.update_state == FACTORY_NAND_UPDATE_STARTING;
        out->update_terminal = out->update_failed = !out->updating;
        COPY(update_title, out->updating ? "Updating system" : "NAND update failed");
        if (out->updating)
            COPY(update_detail, "Opening NAND firmware bundle");
        else
            snprintf(out->update_detail, sizeof(out->update_detail),
                     "Error 0x%08x - rescan or use USB", (unsigned)ctx->nand.update_result);
        COPY(update_owner, "NAND");
        COPY(update_verified, "Validating");
        out->update_revision = ctx->revision;
        return;
    }
    if (ctx->update_source == UPDATE_OTA) {
        out->updating = ota.active || ota.state == ESP_IRIS_JOB_SUCCEEDED;
        out->update_terminal =
            ota.state == ESP_IRIS_JOB_FAILED || ota.state == ESP_IRIS_JOB_CANCELLED;
        out->update_failed = out->update_terminal;
        COPY(update_title, ota.active           ? "Updating firmware"
                           : out->update_failed ? "Firmware update failed"
                                                : "Firmware verified");
        if (ota.active)
            COPY(update_detail, ota.total_size && ota.received_size >= ota.total_size
                                    ? "Awaiting image verification"
                                    : "Receiving application");
        else if (out->update_failed)
            snprintf(out->update_detail, sizeof(out->update_detail),
                     "Error 0x%08x - reconnect and retry", (unsigned)ota.result);
        else
            COPY(update_detail, "Restarting into the application");
        memcpy(out->transfer.job, &ota.job_id, sizeof(ota.job_id));
        out->transfer.total = ota.total_size;
        out->transfer.received = ota.received_size;
        out->transfer.receiving =
            ota.active && (!ota.total_size || ota.received_size < ota.total_size);
        out->progress = vibe_transfer_permille(ota.received_size, ota.total_size);
        COPY(update_owner, iris.transport == ESP_IRIS_TRANSPORT_KIND_USB ? "USB" : "TCP");
        COPY(update_verified, "SHA-256");
        return;
    }
    if (ctx->update_source != UPDATE_SYSTEM ||
        system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE)
        return;
    out->updating = system_active;
    out->update_terminal = !system_active;
    out->update_failed = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED ||
                         system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED;
    COPY(update_title, system_active        ? "Updating system"
                       : out->update_failed ? "Update failed"
                                            : "Update complete");
    const char *detail = "Validating unsigned update plan";
    if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING)
        detail = system.owner == FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE ? "Downloading system component"
                 : system.owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND
                     ? "Reading NAND system component"
                     : "Receiving system component";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED)
        detail = "Component verified";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING)
        detail = "Committing protected system images";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED)
        detail = "System images verified and committed";
    if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING &&
        system.update.component_size &&
        system.update.component_received >= system.update.component_size)
        detail = "Awaiting component verification";
    COPY(update_detail, detail);
    if (out->update_failed)
        snprintf(out->update_detail, sizeof(out->update_detail), "Error 0x%08x - use USB to retry",
                 (unsigned)system.update.result);
    memcpy(out->transfer.job, system.update.operation_id, sizeof(out->transfer.job));
    out->transfer.total = system.total_size;
    out->transfer.received = system.received_size;
    out->transfer.component_id = system.update.active_component_id;
    out->transfer.component_count = system.update.component_count;
    out->transfer.completed_components = system.update.completed_components;
    out->transfer.component_received = system.update.component_received;
    out->transfer.component_size = system.update.component_size;
    out->transfer.receiving = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING &&
                              system.update.component_received < system.update.component_size;
    out->progress = vibe_transfer_permille(system.received_size, system.total_size);
    COPY(update_owner, system.owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND     ? "NAND"
                       : system.owner == FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE ? "Bridge"
                       : out->usb_owner                                     ? "USB"
                                                                            : "TCP");
    COPY(update_verified, "Unsigned");
}
static int service_setting(void *ctx, vibe_settings_command_t command, const char *key, const char *value)
{
    (void)ctx;
    if (command == VIBE_SETTINGS_RESET) {
        esp_iris_ota_status_t ota = {0};
        (void)esp_iris_ota_get_status(&ota);
        if (ota.active || factory_system_update_writer_busy() || s_services.ideas.busy)
            return ESP_ERR_INVALID_STATE;
    }
    return vibe_settings_request(command, key, value);
}
static void service_device_policy(void *ctx, bool asleep, bool rotation_allowed)
{
    (void)ctx; vibe_device_policy(asleep, rotation_allowed);
}
static void service_haptic(void *ctx, unsigned pattern)
{
    (void)ctx; vibe_device_haptic(pattern);
}
static unsigned service_key(void *ctx) { (void)ctx; return vibe_device_poll_key(); }
const vibe_services_t *factory_ui_services(void)
{
    static const vibe_services_t services = {
        .ctx = &s_services,
        .snapshot = service_snapshot,
        .command = service_command,
        .icon = service_icon,
        .title = service_title,
        .preview = service_preview,
        .setting = service_setting, .device_policy = service_device_policy,
        .haptic = service_haptic, .key = service_key,
    };
    return &services;
}
