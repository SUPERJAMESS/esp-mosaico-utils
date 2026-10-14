// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "factory_system_update.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACTORY_NAND_UPDATE_MAX_CANDIDATES 8U
#define FACTORY_NAND_UPDATE_RELEASE_BYTES 48U

typedef enum {
    FACTORY_NAND_SCAN_IDLE = 0,
    FACTORY_NAND_SCAN_RUNNING,
    FACTORY_NAND_SCAN_READY,
    FACTORY_NAND_SCAN_FAILED,
} factory_nand_scan_state_t;

typedef enum {
    FACTORY_NAND_UPDATE_IDLE = 0,
    FACTORY_NAND_UPDATE_STARTING,
    FACTORY_NAND_UPDATE_RUNNING,
    FACTORY_NAND_UPDATE_FAILED,
} factory_nand_update_state_t;

typedef struct {
    char manifest_path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
    char release[FACTORY_NAND_UPDATE_RELEASE_BYTES];
    uint64_t total_size;
    uint8_t component_count;
} factory_nand_update_candidate_t;

typedef struct {
    factory_nand_scan_state_t scan_state;
    factory_nand_update_state_t update_state;
    uint32_t generation;
    esp_err_t scan_result;
    esp_err_t update_result;
    size_t candidate_count;
    size_t invalid_count;
    factory_nand_update_candidate_t candidates[FACTORY_NAND_UPDATE_MAX_CANDIDATES];
} factory_nand_update_snapshot_t;

/* Initialize catalog state before RPC registration; does not start a job.
 * Startup task only; returns allocation errors, or ESP_OK when disabled. */
esp_err_t factory_nand_update_init(void);
/* Queue the NUL-terminated manifest_path below /nand. Copies the path;
 * task context, returns argument/admission errors before accepting work. */
esp_err_t factory_system_update_start_nand(const char *manifest_path);

/* Scan the bounded Recovery bundle catalog below /nand/system-update. The
 * Task-context operation is asynchronous and never formats the filesystem;
 * returns admission errors, then publishes completion in get_snapshot. */
esp_err_t factory_nand_update_request_scan(void);

/* Copy lock-consistent catalog/source status into required snapshot.
 * Nonblocking/task-safe; returns argument/state/lock errors, or not-supported. */
esp_err_t factory_nand_update_get_snapshot(factory_nand_update_snapshot_t *snapshot);

/* Install borrowed NUL-terminated manifest JSON, reading files relative to
 * manifest_path. Caller holds the NAND lease and has verified every package
 * file; call from a worker, as Flash IO blocks. Optional before_commit(ctx)
 * runs after component verification; its error aborts commit. Inputs/ctx must
 * outlive this call. Returns policy/IO/callback errors; success does not reboot. */
esp_err_t factory_nand_install_idea(const char *manifest_path, const char *manifest,
                                    esp_err_t (*before_commit)(void *), void *ctx);

#ifdef __cplusplus
}
#endif
