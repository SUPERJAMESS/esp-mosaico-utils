// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vui.h"
#include "vibe_transfer.h"
#include "vibe_settings_types.h"
#include "vibe_device_types.h"

#define VIBE_AP_MAX 16
#define VIBE_BUNDLE_MAX 16

typedef enum {
    VIBE_HOME,
    VIBE_WIFI,
    VIBE_PASSWORD,
    VIBE_PAIRING,
    VIBE_BRIDGE,
    VIBE_NAND,
    VIBE_CONFIRM,
    VIBE_UPDATE,
    VIBE_RESULT,
    VIBE_IDEA_INFO,
    VIBE_DEVELOP, VIBE_SETTINGS, VIBE_DISPLAY, VIBE_LANGUAGE, VIBE_SHARED,
    VIBE_SHARED_EDITOR, VIBE_ABOUT, VIBE_RESET, VIBE_WELCOME, VIBE_GRID,
    VIBE_OPEN_CONFIRM, VIBE_SLEEP, VIBE_BOOT_TIP, VIBE_DELETE_CONFIRM,
    VIBE_HELP, VIBE_GUIDE,
    VIBE_FIRMWARE_NOTICE, VIBE_FIRMWARE_PROGRESS,
    VIBE_PAGE_COUNT
} vibe_page_t;
typedef enum {
    VIBE_NET_OFFLINE,
    VIBE_NET_CONNECTING,
    VIBE_NET_CONNECTED,
    VIBE_NET_FAILED
} vibe_net_t;
typedef enum {
    VIBE_INSTALL_FIRMWARE,
    VIBE_SCAN_WIFI,
    VIBE_FORGET_WIFI,
    VIBE_CONNECT_WIFI,
    VIBE_OPEN_BRIDGE,
    VIBE_STOP_BRIDGE,
    VIBE_SCAN_NAND,
    VIBE_INSTALL_NAND,
    VIBE_PREFETCH_BRIDGE,
    VIBE_PAUSE_BRIDGE,
    VIBE_DOWNLOAD_IDEA,
    VIBE_CHECK_IDEA,
    VIBE_START_IDEA,
    VIBE_PREVIEW_IDEA,
    VIBE_RESOLVE_IDEA,
    VIBE_CANCEL_IDEA,
    VIBE_SET_LANGUAGE,
    VIBE_OPEN_IDEA, VIBE_DELETE_IDEA, VIBE_SAVE_DEVELOPMENT, VIBE_UPDATE_ALL, VIBE_RUN_DEVELOPMENT
} vibe_command_t;
typedef struct {
    char ssid[33];
    int rssi;
    bool open;
} vibe_ap_t;
typedef struct {
    char release[129], path[256], code[7], version[65], description[513];
    uint64_t bytes;
    unsigned components;
    bool boot, update_available, queued;
} vibe_bundle_t;
typedef struct {
    vibe_net_t network;
    vibe_settings_snapshot_t settings;
    vibe_device_snapshot_t device;
    uint64_t storage_total, storage_used;
    bool development_available, development_saved, cancellable, batch;
    unsigned batch_index, batch_total;
    char batch_code[7];
    char development_name[33], development_version[33], firmware_version[33], mac[18];
    bool credentials_saved, scanning, usb_owner, tcp_owner, session;
    bool release_available, release_installable, firmware_update;
    char release_version[32];
    char ssid[33], ip[16], hostname[48], token[65];
    unsigned tcp_port;
    uint32_t scan_generation;
    size_t ap_count;
    vibe_ap_t aps[VIBE_AP_MAX];
    bool bridge_running;
    char bridge_state[24], bridge_code[16];
    unsigned bridge_seconds;
    int bridge_error;
    uint32_t nand_generation;
    bool nand_scanning;
    int nand_error;
    size_t bundle_count, invalid_bundles;
    vibe_bundle_t bundles[VIBE_BUNDLE_MAX];
    /* A new revision identifies an update state change, including a new job.
     * Active updates own the screen; terminal states are acknowledged by Home. */
    bool idea_busy, idea_loading, idea_ready, idea_downloaded, idea_return_library;
    bool idea_view_only, idea_custom, idea_has_icon;
    uint32_t idea_request;
    vibe_bundle_t idea;
    uint32_t update_revision;
    bool updating, update_terminal, update_failed, update_cancelled;
    unsigned progress; /* permille */
    vibe_transfer_t transfer;
    char update_title[48], update_detail[160], update_owner[16], update_verified[24];
} vibe_snapshot_t;
typedef struct {
    void *ctx;
    void (*snapshot)(void *ctx, vibe_snapshot_t *out);
    int (*command)(void *ctx, vibe_command_t command, const char *first, const char *second);
    void *(*icon)(void *ctx, const char *code, size_t *size);
    void *(*title)(void *ctx, const char *code, size_t *size);
    /* malloc-owned copies: title/body RGB565, then encoded icon PNG. */
    void *(*preview)(void *ctx, uint32_t request, unsigned slot, size_t *size);
    int (*setting)(void *ctx, vibe_settings_command_t command, const char *key, const char *value);
    void (*device_policy)(void *ctx, bool asleep, bool rotation_allowed);
    void (*haptic)(void *ctx, unsigned pattern);
    unsigned (*key)(void *ctx);
} vibe_services_t;
typedef struct vibe_ui vibe_ui_t;
typedef struct {
    vibe_page_t page;
    size_t password_length;
    bool password_visible, download_pending;
    bool rate_valid;
    uint64_t rate_bytes_per_second;
    char idea_code[7];
    unsigned grid_page, language;
    bool asleep, pending_delete;
} vibe_ui_status_t;

/* Create this backend's single controller on the render task. Copies services;
 * services->ctx and ui must outlive the controller. out is component-owned,
 * stable storage (PSRAM on device), including on partial initialization error;
 * never free it. A live controller cannot be initialized twice. */
vui_err_t vibe_ui_create(vui_handle_t ui, const vibe_services_t *services, vibe_ui_t **out);
/* Copy observable UI state without exposing secrets. Render task only. */
void vibe_ui_get_status(const vibe_ui_t *state, vibe_ui_status_t *out);
/* Detach callbacks and clear secrets on the render task before UI destruction.
 */
void vibe_ui_deinit(vibe_ui_t *state);
/* Open the six-digit download page on the render task for live state.
 * Returns the service command result; never call directly from an Iris worker.
 */
int vibe_ui_open_bridge(vibe_ui_t *state);
/* Refresh state from copied service callbacks and update the current page.
 * state must be a live controller; call only on the render task. */
void vibe_ui_poll(vibe_ui_t *state);
