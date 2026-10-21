#pragma once
#include "vibe_ui.h"
#include "vibe_home_motion.h"

/* Render-task-owned controller state. Never exposed to device/PC callers. */
typedef struct {
    bool active, settling, candidate, blocked, vertical, live;
    vibe_page_t origin;
    unsigned grid, count;
    uint32_t generation;
    char codes[VIBE_BUNDLE_MAX][7];
    int anchor_x, base_shift, slop, shift, offset, target;
    int settle_from, settle_to, settle_tangent;
    uint64_t settle_start;
    unsigned settle_duration;
    void *timer;
} vibe_pager_t;
struct vibe_ui {
    vui_handle_t ui;
    vibe_services_t services;
    vibe_snapshot_t snapshot;
    vibe_page_t page;
    vui_list_t networks, bundles;
    void *timer;
    void *home_timer;
    vibe_home_motion_t home_motion;
    bool download_pending, password_visible, preview_active, batch_cancel_pending, result_cancelled, result_failed, result_can_open;
    vibe_page_t preview_return, format_return;
    bool nand_prompt_dismissed;
    bool prefetch_attempted;
    bool rendered_release;
    char notified_firmware[32];
    enum { CHECK_IDLE, CHECK_PENDING, CHECK_CURRENT, CHECK_FAILED, CHECK_OFFLINE } firmware_check;
    uint32_t firmware_check_generation;
    char selected_ssid[33], password_projection[65], password[65];
    char idea_code[7];
    vibe_bundle_t selected_bundle;
    uint32_t acknowledged_update, rendered_preview;
    bool preview_loading_title_ready;
    bool preview_check_updates, result_return_detail;
    bool inline_intro_due, inline_intro_pending, inline_intro_ready;
    bool detail_followup_due;
    vibe_command_t detail_followup;
    vibe_rate_t rate;
    uint64_t page_enter_ms, next_poll_ms, activity_ms, hold_started_ms, suppress_until, undo_until;
    uint64_t last_release_ms, nav_badge_until;
    bool open_waiting, open_unsaved, run_development_pending, code_submitted, shared_detail_reveal, shared_system_entry, shared_rename_mode;
    unsigned guide_topic, help_mode, help_topic_index;
    bool fix_return_active, shared_settings_return;
    unsigned grid_page, grid_rendered_page, pager_state, shared_page, last_shared_user_count;
    bool grid_titles_pending, open_title_ready;
    unsigned open_title_language;
    unsigned grid_title_ready_mask;
    vibe_pager_t pager;
    unsigned haptic_revision, last_language, grid_generation, settings_revision, motion_revision;
    bool gesture_consumed, pointer_page_ready, hold_consumed, touch_haptic_armed;
    bool home_capture;
    vibe_page_t wifi_return;
    vibe_page_t pointer_page;
    bool pointer_down, first_run, shared_edit_value, shared_reveal, undo_shared, shared_wait;
    int32_t pointer_x, pointer_y;
    int32_t last_release_x, last_release_y;
    bool have_release;
    bool pointer_network_open;
    char pointer_key[33];
    vibe_page_t return_page, wake_page, boot_page;
    char delete_code[7], undo_key[16], shared_key[16], shared_orig_key[16], shared_value[257];
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
void vibe_product_cancel_pager(vibe_ui_t *s);
size_t vibe_bundle_count(const vibe_ui_t *s);
size_t vibe_bundle_index(const vibe_ui_t *s, size_t visible);
