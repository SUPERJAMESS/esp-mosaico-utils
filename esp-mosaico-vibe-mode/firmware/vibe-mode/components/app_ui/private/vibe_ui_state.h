#pragma once
#include "vibe_ui.h"

/* Render-task-owned controller state. Never exposed to device/PC callers. */
struct vibe_ui {
    esp_gsp_handle_t ui;
    vibe_services_t services;
    vibe_snapshot_t snapshot;
    vibe_page_t page;
    esp_gsp_list_t networks, bundles;
    void *timer;
    bool download_pending, password_visible, preview_active;
    vibe_page_t preview_return;
    bool prefetch_attempted;
    char selected_ssid[33], password_projection[65], password[65];
    char idea_code[7];
    vibe_bundle_t selected_bundle;
    uint32_t acknowledged_update, rendered_preview;
    vibe_rate_t rate;
    unsigned tick;
};
