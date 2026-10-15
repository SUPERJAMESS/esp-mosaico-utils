// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available, installable, checking;
    uint32_t generation;
    int error;
    char file[129], sha256[65];
    uint32_t size;
    char version[32];
} factory_update_check_snapshot_t;

/* Manual requests bypass the interval; concurrent requests share the running check. */
void factory_update_check_request(bool manual);
void factory_update_check_snapshot(factory_update_check_snapshot_t *out);
