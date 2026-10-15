#pragma once
#include "vibe_ui.h"

/* Render-task-owned controller state. Never exposed to device/PC callers. */
struct vibe_ui {
    vui_handle_t ui;
    vibe_services_t services;
    vibe_snapshot_t snapshot;
    vibe_page_t page;
    vui_list_t networks, bundles;
    void *timer;
    bool download_pending, password_visible, preview_active, batch_cancel_pending, result_cancelled;
    vibe_page_t preview_return;
    bool prefetch_attempted;
    bool rendered_release;
    char notified_firmware[32];
    enum { CHECK_IDLE, CHECK_PENDING, CHECK_CURRENT, CHECK_FAILED, CHECK_OFFLINE } firmware_check;
    uint32_t firmware_check_generation;
    char selected_ssid[33], password_projection[65], password[65];
    char idea_code[7];
    vibe_bundle_t selected_bundle;
    uint32_t acknowledged_update, rendered_preview;
    vibe_rate_t rate;
    unsigned tick, page_enter_tick;
    bool open_waiting, open_unsaved;
    unsigned grid_page, pager_state, shared_page, activity_tick, hold_tick, suppress_until, undo_until;
    unsigned haptic_revision, last_language, grid_generation, settings_revision, motion_revision;
    bool gesture_consumed, pointer_page_ready, hold_consumed;
    vibe_page_t pointer_page;
    bool pointer_down, first_run, shared_edit_value, shared_reveal, undo_shared, shared_wait;
    int32_t pointer_x, pointer_y;
    vibe_page_t return_page, wake_page;
    char delete_code[7], undo_key[16], shared_key[16], shared_value[257];
};

const char *vibe_translate(vibe_ui_t *s, const char *text);
void vibe_show(vibe_ui_t *s, vibe_page_t page);
int vibe_command(vibe_ui_t *s, vibe_command_t cmd, const char *a, const char *b);
void vibe_error(vibe_ui_t *s, const char *title, int error);
bool vibe_product_event(vibe_ui_t *s, const vui_event_t *event);
void vibe_product_poll(vibe_ui_t *s);
void vibe_product_tick(vibe_ui_t *s);
bool vibe_product_leave(vibe_ui_t *s, vibe_page_t next);
void vibe_product_init(vibe_ui_t *s);
size_t vibe_bundle_count(const vibe_ui_t *s);
size_t vibe_bundle_index(const vibe_ui_t *s, size_t visible);
