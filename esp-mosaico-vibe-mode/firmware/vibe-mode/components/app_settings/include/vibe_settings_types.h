// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_SHARED_USER_MAX 16
/* Wi-Fi credentials do not consume the user's existing shared slots. */
#define VIBE_SHARED_MAX (VIBE_SHARED_USER_MAX + 2)
#define VIBE_SHARED_KEY_BYTES 16
#define VIBE_SHARED_VALUE_BYTES 257

typedef struct {
    char key[VIBE_SHARED_KEY_BYTES], value[VIBE_SHARED_VALUE_BYTES];
} vibe_shared_entry_t;
typedef struct {
    /* 1 = English (persisted default), 2 = Simplified Chinese. */
    unsigned language, brightness, screen_off_seconds;
    bool auto_rotate, haptics, open_confirm;
    uint32_t revision;
    bool busy;
    int error;
    size_t shared_count;
    vibe_shared_entry_t shared[VIBE_SHARED_MAX];
} vibe_settings_snapshot_t;
typedef enum {
    VIBE_SETTINGS_LANGUAGE, VIBE_SETTINGS_BRIGHTNESS, VIBE_SETTINGS_TIMEOUT,
    VIBE_SETTINGS_ROTATE, VIBE_SETTINGS_HAPTICS, VIBE_SETTINGS_OPEN_CONFIRM,
    VIBE_SETTINGS_SHARED_SET, VIBE_SETTINGS_SHARED_DELETE, VIBE_SETTINGS_RESET
} vibe_settings_command_t;
