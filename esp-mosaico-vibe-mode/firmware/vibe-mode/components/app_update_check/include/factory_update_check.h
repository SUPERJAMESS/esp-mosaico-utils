// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available, installable;
    char file[129], sha256[65];
    uint32_t size;
    char version[32];
} factory_update_check_snapshot_t;

/* Requests are asynchronous and coalesced while a check is running. */
void factory_update_check_request(void);
void factory_update_check_snapshot(factory_update_check_snapshot_t *out);
