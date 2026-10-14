// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FACTORY_IDEAS_MAX_ITEMS 16U

typedef enum {
    IDEAS_SCAN,
    IDEAS_DOWNLOAD,
    IDEAS_CHECK,
    IDEAS_INSTALL,
    IDEAS_PREVIEW,
    IDEAS_START,
    IDEAS_RESOLVE,
    IDEAS_DISMISS,
    IDEAS_ROW,
    IDEAS_RELOCALIZE
} factory_ideas_command_t;
/* Local UI state; never serialized into the Iris protocol. */
typedef enum {
    IDEAS_PHASE_NONE,
    IDEAS_PHASE_DOWNLOAD,
    IDEAS_PHASE_VERIFY_FILES,
    IDEAS_PHASE_CHECK_IMAGE,
    IDEAS_PHASE_SAVE,
    IDEAS_PHASE_COMPLETE
} factory_ideas_phase_t;
typedef struct {
    char code[7], application_id[65], release_id[65], version[65], name[129];
    char description[513], presentation[193], icon[193], latest_version[65];
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
    factory_ideas_phase_t phase;
    unsigned files_verified, file_count;
    size_t count, invalid;
    factory_idea_t items[FACTORY_IDEAS_MAX_ITEMS];
    uint32_t preview_id;
    bool resolving, preview_ready, already_downloaded, return_library, view_only, custom_display,
        has_icon;
    factory_idea_t preview;
} factory_ideas_snapshot_t;

/* Reserve one 16 KiB HP SRAM worker stack and queue the initial scan.
 * Call during serialized startup, after network initialization. Idempotent;
 * failure releases partial resources and permits retry. No RTC/PSRAM fallback. */
esp_err_t factory_ideas_init(void);
/* Queue command, copying the six-digit code when the command needs one.
 * Scan/relocalize accept NULL; preview/check/install require code. Returns busy or error.
 */
esp_err_t factory_ideas_request(factory_ideas_command_t command, const char *code);
/* Confirm or dismiss the current immutable preview identified by request_id.
 * download=true saves to NAND; false dismisses. Stale/duplicate IDs are
 * rejected. */
esp_err_t factory_ideas_confirm(uint32_t request_id, bool download);
/* Invalidate an in-flight preview or asynchronously release a ready preview.
 * Never starts a download; completion of unrelated active IO is not cancelled.
 */
esp_err_t factory_ideas_cancel(void);
/* Copy current library/preview status into required out without waiting.
 * Returns timeout on snapshot contention; safe for render-task polling. */
esp_err_t factory_ideas_snapshot(factory_ideas_snapshot_t *out);
/* Return a malloc-owned icon copy for code, setting required size.
 * No filesystem IO; returns NULL if absent, busy or allocation failed. free it.
 */
void *factory_ideas_copy_icon(const char *code, size_t *size);
/* Nonblocking visible-row cache. A miss queues the serialized worker; the
 * next generation refresh supplies QOI bytes rendered from the name block. */
void *factory_ideas_copy_title(const char *code, size_t *size);

/* Copy request_id preview slot (0 title RGB565, 1 body RGB565, 2 PNG).
 * Sets required size; caller frees the result. May wait for the snapshot lock.
 */
void *factory_ideas_copy_preview(uint32_t request_id, unsigned slot, size_t *size);
