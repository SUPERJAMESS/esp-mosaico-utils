// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "vibe_settings_types.h"

/* Initialize sysmeta-backed preferences before network startup. Completes any
 * interrupted reset. Never touches NAND, app partitions, identity or Iris keys. */
esp_err_t vibe_settings_init(void);
/* Copy status without waiting; caller retains its previous copy on contention. */
esp_err_t vibe_settings_snapshot(vibe_settings_snapshot_t *out);
/* Nonblocking admission, copies arguments. NVS writes run on a dedicated task.
 * Numeric commands use value; shared commands use key/value. Reset reboots into
 * Vibe Mode only after the allowlisted namespaces have been cleared. */
esp_err_t vibe_settings_request(vibe_settings_command_t command, const char *key,
                                const char *value);
unsigned vibe_settings_language(void);
bool vibe_shared_key_valid(const char *key);
bool vibe_shared_key_secret(const char *key);
/* Task-context read for application integrations. No logging of shared values. */
esp_err_t vibe_shared_get(const char *key, char *value, size_t size);
