#include "esp_attr.h"
// SPDX-License-Identifier: Apache-2.0
#include "factory_ui.h"
#include "factory_ui_dispatch.h"
#include "factory_ui_input.h"
#include "vibe_ui.h"
#include "board_display.h"
#include "vibe_bundle.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_check.h"
#include "esp_iris.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "factory_ideas.h"
#include "factory_network.h"
#include "factory_nand_update.h"
#include "factory_system_update.h"
#include "iris_bridge.h"
#include "iris_screen_mirror.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "vibe_ui";
static EXT_RAM_BSS_ATTR vibe_ui_t s_ui;
static EXT_RAM_BSS_ATTR factory_nand_update_snapshot_t s_nand;
static EXT_RAM_BSS_ATTR factory_ideas_snapshot_t s_ideas;
static uint32_t s_ideas_revision;
static esp_iris_system_update_phase_t s_phase;
static factory_nand_update_state_t s_nand_state;
static uint32_t s_ota_job;
static int s_ota_state;
static uint32_t s_revision;
static uint8_t s_operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
static enum { UPDATE_NONE, UPDATE_OTA, UPDATE_SYSTEM, UPDATE_NAND, UPDATE_IDEAS } s_update_source;

static int service_command(void *ctx, vibe_command_t cmd, const char *a, const char *b)
{
    (void)ctx;
    switch (cmd) {
    case VIBE_SCAN_WIFI: return factory_network_request_scan();
    case VIBE_FORGET_WIFI: return factory_network_forget();
    case VIBE_CONNECT_WIFI: return factory_network_connect(a, b);
    case VIBE_OPEN_BRIDGE: return ESP_OK;
    case VIBE_PREFETCH_BRIDGE: return ESP_OK;
    case VIBE_PAUSE_BRIDGE: iris_bridge_set_active(false); return ESP_OK;
    case VIBE_STOP_BRIDGE: iris_bridge_stop(); return ESP_OK;
    case VIBE_SCAN_NAND: return factory_ideas_request(IDEAS_SCAN, NULL);
    case VIBE_INSTALL_NAND: return factory_ideas_request(IDEAS_INSTALL, a);
    case VIBE_RESOLVE_IDEA: return factory_ideas_request(IDEAS_RESOLVE, a);
    case VIBE_CANCEL_IDEA: return factory_ideas_cancel();
    case VIBE_DOWNLOAD_IDEA: return factory_ideas_confirm(b ? strtoul(b, NULL, 10) : 0, true);
    case VIBE_CHECK_IDEA: return factory_ideas_request(IDEAS_CHECK, a);
    case VIBE_START_IDEA: return factory_ideas_request(IDEAS_START, a);
    case VIBE_PREVIEW_IDEA: return factory_ideas_request(IDEAS_PREVIEW, a);
    default: return ESP_ERR_INVALID_ARG;
    }
}
static void *service_icon(void *ctx, const char *code, size_t *size)
{
    (void)ctx;
    return factory_ideas_copy_icon(code, size);
}
static void *service_preview(void *ctx, uint32_t request, unsigned slot, size_t *size)
{
    (void)ctx;
    return factory_ideas_copy_preview(request, slot, size);
}
#define COPY(field, value) snprintf(out->field, sizeof(out->field), "%s", value)
static void service_snapshot(void *ctx, vibe_snapshot_t *out)
{
    (void)ctx;
    factory_network_snapshot_t network = {0};
    esp_iris_status_t iris = {0};
    iris_bridge_snapshot_t bridge = {0};
    (void)factory_network_get_snapshot(&network);
    (void)esp_iris_get_status(&iris);
    iris_bridge_get_snapshot(&bridge);
    out->network = network.state == FACTORY_NETWORK_CONNECTED ? VIBE_NET_CONNECTED :
        network.state == FACTORY_NETWORK_CONNECTING ? VIBE_NET_CONNECTING :
        network.state == FACTORY_NETWORK_FAILED ? VIBE_NET_FAILED : VIBE_NET_OFFLINE;
    out->credentials_saved = network.credentials_saved;
    out->scanning = network.scanning;
    out->session = iris.session_ready;
    out->usb_owner = iris.session_ready && iris.transport == ESP_IRIS_TRANSPORT_KIND_USB;
    out->tcp_owner = iris.session_ready && iris.transport == ESP_IRIS_TRANSPORT_KIND_TCP;
    COPY(ssid, network.ssid); COPY(ip, network.ip); COPY(hostname, network.hostname);
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
    COPY(bridge_code, bridge.code); COPY(bridge_state, bridge.state);
    out->bridge_seconds = bridge.expires_in_ms / 1000;
    out->bridge_error = bridge.error;
    (void)factory_nand_update_get_snapshot(&s_nand);
    (void)factory_ideas_snapshot(&s_ideas);
    out->nand_generation = s_ideas.generation;
    out->nand_scanning = s_ideas.scanning;
    out->nand_error = s_ideas.scan_error;
    out->bundle_count = s_ideas.count < VIBE_BUNDLE_MAX ? s_ideas.count : VIBE_BUNDLE_MAX;
    out->invalid_bundles = s_ideas.invalid;
    out->idea_busy = s_ideas.busy;
    out->idea_loading = s_ideas.resolving;
    out->idea_ready = s_ideas.preview_ready;
    out->idea_view_only = s_ideas.view_only;
    out->idea_custom = s_ideas.custom_display;
    out->idea_has_icon = s_ideas.has_icon;
    out->idea_downloaded = s_ideas.already_downloaded;
    out->idea_return_library = s_ideas.return_library;
    out->idea_request = s_ideas.preview_id;
    snprintf(out->idea.code, sizeof(out->idea.code), "%s", s_ideas.preview.code);
    snprintf(out->idea.release, sizeof(out->idea.release), "%s", s_ideas.preview.name);
    snprintf(out->idea.version, sizeof(out->idea.version), "%s", s_ideas.preview.version);
    snprintf(out->idea.description, sizeof(out->idea.description), "%s", s_ideas.preview.description);
    out->idea.bytes = s_ideas.preview.bytes;
    for (size_t i = 0; i < out->bundle_count; ++i) {
        const factory_idea_t *c = &s_ideas.items[i];
        snprintf(out->bundles[i].release, sizeof(out->bundles[i].release), "%s", c->name);
        snprintf(out->bundles[i].path, sizeof(out->bundles[i].path), "%s", c->code);
        snprintf(out->bundles[i].code, sizeof(out->bundles[i].code), "%s", c->code);
        snprintf(out->bundles[i].version, sizeof(out->bundles[i].version), "%s", c->version);
        snprintf(out->bundles[i].description, sizeof(out->bundles[i].description), "%s", c->description);
        out->bundles[i].boot = c->boot; out->bundles[i].update_available = c->update_available;
        out->bundles[i].bytes = c->bytes; out->bundles[i].components = c->components;
    }
    esp_iris_ota_status_t ota = {0};
    factory_system_update_status_t system = {0};
    (void)esp_iris_ota_get_status(&ota);
    (void)factory_system_update_get_status(&system);
    const bool nand_changed = s_nand_state != s_nand.update_state;
    const bool ota_changed = ota.job_id != s_ota_job || (int)ota.state != s_ota_state;
    const bool system_changed = s_phase != system.update.phase ||
        memcmp(s_operation_id, system.update.operation_id, sizeof(s_operation_id)) != 0;
    if (nand_changed || ota_changed || system_changed) ++s_revision;
    s_nand_state = s_nand.update_state;
    s_ota_job = ota.job_id; s_ota_state = ota.state;
    s_phase = system.update.phase;
    memcpy(s_operation_id, system.update.operation_id, sizeof(s_operation_id));
    const bool system_active = system.update.phase != ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE &&
        system.update.phase < ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED;
    if (nand_changed && s_nand.update_state == FACTORY_NAND_UPDATE_FAILED) s_update_source = UPDATE_NAND;
    if (ota_changed && ota.job_id) s_update_source = UPDATE_OTA;
    if (system_changed && system.update.phase != ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE) s_update_source = UPDATE_SYSTEM;
    /* Active writers supersede retained failures from all other sources. */
    if (s_nand.update_state == FACTORY_NAND_UPDATE_STARTING) s_update_source = UPDATE_NAND;
    if (ota.active) s_update_source = UPDATE_OTA;
    if (system_active) s_update_source = UPDATE_SYSTEM;
    if (s_ideas.revision != s_ideas_revision) {
        s_ideas_revision = s_ideas.revision; ++s_revision; s_update_source = UPDATE_IDEAS;
    }
    if (s_ideas.busy && (s_ideas.command == IDEAS_DOWNLOAD || s_ideas.command == IDEAS_INSTALL)) s_update_source = UPDATE_IDEAS;
    out->updating = out->update_terminal = out->update_failed = false;
    out->progress = 0;
    memset(&out->transfer, 0, sizeof(out->transfer));
    out->transfer.now_ms = (uint64_t)esp_timer_get_time() / 1000;
    out->transfer.source = (uint8_t)s_update_source;
    out->update_revision = s_revision;
    if (s_update_source == UPDATE_IDEAS) {
        out->updating = s_ideas.busy && (s_ideas.command == IDEAS_DOWNLOAD || s_ideas.command == IDEAS_INSTALL);
        out->update_terminal = s_ideas.terminal;
        out->update_failed = s_ideas.failed;
        COPY(update_title, s_ideas.title); COPY(update_detail, s_ideas.detail);
        COPY(update_owner, s_ideas.command == IDEAS_INSTALL ? "NAND" : "Cloud");
        COPY(update_verified, "SHA-256");
        out->transfer.total = s_ideas.total; out->transfer.received = s_ideas.received;
        memcpy(out->transfer.job, &s_ideas.revision, sizeof(s_ideas.revision));
        out->transfer.receiving = out->updating && s_ideas.command == IDEAS_DOWNLOAD;
        if (s_ideas.command == IDEAS_INSTALL && system_active) {
            out->transfer.total = system.total_size; out->transfer.received = system.received_size;
            out->transfer.component_id = system.update.active_component_id;
            out->transfer.component_count = system.update.component_count;
            out->transfer.completed_components = system.update.completed_components;
            out->transfer.component_size = system.update.component_size;
            out->transfer.component_received = system.update.component_received;
            out->transfer.receiving = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING;
            COPY(update_detail, system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING ?
                "Selecting boot application" : "Installing downloaded files");
        }
        return;
    }
    if (s_update_source == UPDATE_NAND) {
        out->updating = s_nand.update_state == FACTORY_NAND_UPDATE_STARTING;
        out->update_terminal = out->update_failed = !out->updating;
        COPY(update_title, out->updating ? "Updating system" : "NAND update failed");
        if (out->updating) COPY(update_detail, "Opening NAND firmware bundle");
        else snprintf(out->update_detail, sizeof(out->update_detail), "Error 0x%08x - rescan or use USB", (unsigned)s_nand.update_result);
        COPY(update_owner, "NAND"); COPY(update_verified, "Validating");
        out->update_revision = s_revision;
        return;
    }
    if (s_update_source == UPDATE_OTA) {
        out->updating = ota.active || ota.state == ESP_IRIS_JOB_SUCCEEDED;
        out->update_terminal = ota.state == ESP_IRIS_JOB_FAILED || ota.state == ESP_IRIS_JOB_CANCELLED;
        out->update_failed = out->update_terminal;
        COPY(update_title, ota.active ? "Updating firmware" : out->update_failed ? "Firmware update failed" : "Firmware verified");
        if (ota.active) COPY(update_detail, ota.total_size && ota.received_size >= ota.total_size ? "Awaiting image verification" : "Receiving application");
        else if (out->update_failed) snprintf(out->update_detail, sizeof(out->update_detail), "Error 0x%08x - reconnect and retry", (unsigned)ota.result);
        else COPY(update_detail, "Restarting into the application");
        memcpy(out->transfer.job, &ota.job_id, sizeof(ota.job_id));
        out->transfer.total = ota.total_size;
        out->transfer.received = ota.received_size;
        out->transfer.receiving = ota.active && (!ota.total_size || ota.received_size < ota.total_size);
        out->progress = vibe_transfer_permille(ota.received_size, ota.total_size);
        COPY(update_owner, iris.transport == ESP_IRIS_TRANSPORT_KIND_USB ? "USB" : "TCP");
        COPY(update_verified, "SHA-256");
        return;
    }
    if (s_update_source != UPDATE_SYSTEM || system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE) return;
    out->updating = system_active;
    out->update_terminal = !system_active;
    out->update_failed = system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED || system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED;
    COPY(update_title, system_active ? "Updating system" : out->update_failed ? "Update failed" : "Update complete");
    const char *detail = "Validating unsigned update plan";
    if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING)
        detail = system.owner == FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE ? "Downloading system component" : system.owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND ? "Reading NAND system component" : "Receiving system component";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED) detail = "Component verified";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING) detail = "Committing protected system images";
    else if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED) detail = "System images verified and committed";
    if (system.update.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING &&
        system.update.component_size && system.update.component_received >= system.update.component_size)
        detail = "Awaiting component verification";
    COPY(update_detail, detail);
    if (out->update_failed) snprintf(out->update_detail, sizeof(out->update_detail), "Error 0x%08x - use USB to retry", (unsigned)system.update.result);
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
    COPY(update_owner, system.owner == FACTORY_SYSTEM_UPDATE_OWNER_NAND ? "NAND" : system.owner == FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE ? "Bridge" : out->usb_owner ? "USB" : "TCP");
    COPY(update_verified, "Unsigned");
}
esp_gsp_handle_t factory_ui_handle(void) { return s_ui.ui; }
esp_err_t factory_ui_start(void)
{
    esp_display_present_target_config_t display;
    ESP_RETURN_ON_ERROR(board_display_init(&display), TAG, "display");
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(board_touch_init(&touch), TAG, "touch");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen backend");
    esp_gsp_config_t config;
    ESP_RETURN_ON_ERROR(vibe_bundle_open(&config), TAG, "embedded GSP bundle");
    /* GSPC counts the template's image once. The library needs independent
     * targets for every retained row, including rows recycled while scrolling. */
    if (esp_gsp_config_set(&config, ESP_GSP_FIELD_DEFAULT_DYNAMIC_IMAGE_SLOTS,
                           VIBE_BUNDLE_MAX) != ESP_GSP_CONFIG_SET_OK)
        return ESP_ERR_INVALID_STATE;
    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display; lcd.touch = touch;
    esp_gsp_handle_t ui;
    ESP_RETURN_ON_ERROR(esp_gsp_esp_lcd_start(&config, &lcd, &ui), TAG, "GSP start");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(ui), TAG, "screen attach");
    ESP_RETURN_ON_ERROR(factory_ideas_init(), TAG, "Ideas library");
    const vibe_services_t services = {.snapshot = service_snapshot, .command = service_command, .icon = service_icon, .preview = service_preview};
    ESP_RETURN_ON_ERROR(factory_ui_dispatch_start(ui, &s_ui, &services), TAG, "Vibe UI");
    ESP_LOGI(TAG, "Vibe Mode GSP UI ready at 480x480");
    return ESP_OK;
}
esp_err_t factory_ui_open_bridge(void)
{
    return factory_ui_dispatch_open_bridge();
}
