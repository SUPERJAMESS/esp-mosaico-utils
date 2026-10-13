// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ideas_package.h"

typedef enum { IDEAS_SCAN, IDEAS_DOWNLOAD, IDEAS_CHECK, IDEAS_INSTALL, IDEAS_PREVIEW, IDEAS_START, IDEAS_RESOLVE, IDEAS_DISMISS } factory_ideas_command_t;
typedef struct {
    char code[7], application_id[65], release_id[65], version[65], name[129];
    char description[513], presentation[65], icon[65], latest_version[65];
    uint64_t bytes;
    unsigned components;
    bool boot, update_available;
} factory_idea_t;
typedef struct {
    uint32_t generation, revision;
    bool busy, terminal, failed, scanning;
    factory_ideas_command_t command;
    esp_err_t error, scan_error;
    char title[48], detail[160];
    uint64_t total, received;
    size_t count, invalid;
    factory_idea_t items[IDEAS_MAX_ITEMS];
    uint32_t preview_id;
    bool resolving, preview_ready, already_downloaded, return_library, view_only, custom_display, has_icon;
    factory_idea_t preview;
} factory_ideas_snapshot_t;

esp_err_t factory_ideas_init(void);
esp_err_t factory_ideas_request(factory_ideas_command_t command, const char *code);
/* Accept only the currently displayed, immutable release. */
esp_err_t factory_ideas_confirm(uint32_t request_id, bool download);
esp_err_t factory_ideas_cancel(void);
esp_err_t factory_ideas_snapshot(factory_ideas_snapshot_t *out);
/* malloc-compatible image copy; no filesystem IO on the render task. */
void *factory_ideas_copy_icon(const char *code, size_t *size);

/* Preview slots: 0 title RGB565, 1 description RGB565, 2 encoded PNG. */
void *factory_ideas_copy_preview(uint32_t request_id, unsigned slot, size_t *size);
