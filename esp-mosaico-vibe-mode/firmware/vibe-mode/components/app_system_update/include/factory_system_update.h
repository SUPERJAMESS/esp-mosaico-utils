#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_iris_system_update.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACTORY_SYSTEM_UPDATE_FILENAME_BYTES 129U
#define FACTORY_SYSTEM_UPDATE_PATH_BYTES 256U

typedef enum {
    FACTORY_SYSTEM_UPDATE_OWNER_NONE = 0,
    FACTORY_SYSTEM_UPDATE_OWNER_ESP_IRIS,
    FACTORY_SYSTEM_UPDATE_OWNER_BRIDGE,
    FACTORY_SYSTEM_UPDATE_OWNER_NAND,
    FACTORY_SYSTEM_UPDATE_OWNER_READER,
} factory_system_update_owner_t;

typedef struct {
    factory_system_update_owner_t owner;
    esp_iris_system_update_status_t update;
    /* Local, lock-consistent payload totals; includes the active component. */
    uint64_t total_size;
    uint64_t received_size;
    uint64_t completed_size;
} factory_system_update_status_t;

/* Register the Recovery-resident, product-owned Flash-policy backend.
 * A Recovery self-update is staged completely in PSRAM before the backend
 * temporarily unlocks and rewrites the running factory partition.
 * When the backend is disabled, this remains a successful no-op and the
 * read-only System Inventory service is still available. */
esp_err_t factory_system_update_register(void);

/* Copy a lock-consistent transaction snapshot into required status.
 * Task context; returns invalid-argument for NULL. */
esp_err_t factory_system_update_get_status(factory_system_update_status_t *status);
/* Return whether a writer owns the backend or a committed layout needs reboot.
 * Nonblocking admission probe; it does not reserve the writer. */
bool factory_system_update_writer_busy(void);
/* Reserve a stable application/layout snapshot against new writers. Caller
 * rechecks OTA activity and hashes before publishing a copied NAND generation. */
esp_err_t factory_system_update_read_acquire(void);
void factory_system_update_read_release(void);

/* Reserve the writer for owner and a nonzero operation_id before source IO.
 * Copies ID; returns invalid-state on contention. The owning task serializes
 * subsequent calls. Abort releases resources; commit may retain ownership
 * until reboot so SDK partition descriptors cannot become stale. */
esp_err_t factory_system_update_source_reserve(
    factory_system_update_owner_t owner,
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES]);
/* Reserve an offline Ideas transaction using a validated target-table layout.
 * operation_id is a required nonzero 16-byte ID; returns admission error.
 * May replace a committed Idea while other writers await a partition reload. */
esp_err_t factory_system_update_source_reserve_idea(const uint8_t operation_id[16]);

/* Validate bounded manifest bytes for the reserved owner/operation_id.
 * Borrows input only for this call; no transport may authorize target addresses. */
esp_err_t factory_system_update_source_prepare(
    factory_system_update_owner_t owner, const uint8_t *manifest, size_t manifest_size,
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES]);
/* Validate a borrowed parsed manifest for the reserved Bridge operation_id.
 * allow_bootloader is trusted local policy, never supplied by remote input.
 * The generic prepare API always rejects Bridge bootloader replacement. */
esp_err_t factory_system_update_source_prepare_bridge(
    const cJSON *manifest, const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
    bool allow_bootloader);
/* Return the prepared component count for owner, or zero without ownership.
 * Only the serialized owning source may enumerate an active plan. */
size_t factory_system_update_source_component_count(factory_system_update_owner_t owner);
/* Copy descriptor and filename at index for the prepared owner.
 * Both outputs are required; returns invalid-argument for ownership/range errors. */
esp_err_t
factory_system_update_source_component(factory_system_update_owner_t owner, size_t index,
                                       esp_iris_system_update_component_t *component,
                                       char filename[FACTORY_SYSTEM_UPDATE_FILENAME_BYTES]);
/* Begin the exact prepared component for owner; may erase its target.
 * Call only from the serialized writer task. Returns policy/Flash errors. */
esp_err_t
factory_system_update_source_begin_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component);
/* Write size bytes from borrowed data at the next sequential offset.
 * owner/component must match the active transaction; returns write/state errors. */
esp_err_t
factory_system_update_source_write_component(factory_system_update_owner_t owner,
                                             const esp_iris_system_update_component_t *component,
                                             uint32_t offset, const uint8_t *data, size_t size);
/* Finish component, checking its size, actual_sha256 and image/readback rules.
 * All inputs are borrowed for this call; the owning task serializes access. */
esp_err_t factory_system_update_source_end_component(
    factory_system_update_owner_t owner, const esp_iris_system_update_component_t *component,
    const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES]);
/* Return whether owner entered a critical Bridge commit requiring reboot.
 * A failed critical commit still requires reloading partition descriptors. */
bool factory_system_update_source_needs_restart(factory_system_update_owner_t owner);
/* Commit the matching Ideas operation_id without restarting Vibe Mode.
 * Blocks for verification/Flash IO; other writers wait for partition reload. */
esp_err_t factory_system_update_source_commit_deferred(const uint8_t operation_id[16]);
/* Commit the prepared owner/operation_id after every component verified.
 * May schedule restart; returns commit/readback/persistence errors. */
esp_err_t
factory_system_update_source_commit(factory_system_update_owner_t owner,
                                    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES]);
/* Abort the matching owner/operation_id with reason and release resources.
 * Does not undo completed Flash writes or clear required partition reload. */
void factory_system_update_source_abort(
    factory_system_update_owner_t owner,
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES], esp_err_t reason);

#ifdef __cplusplus
}
#endif
