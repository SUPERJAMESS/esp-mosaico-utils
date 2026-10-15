// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "vui.h"

#define VIBE_HOME_DOT_COUNT 49
#define VIBE_HOME_DOTS_X 180
#define VIBE_HOME_DOTS_Y 60
#define VIBE_HOME_DOTS_SIZE 120

/* Product animation state, owned by the single UI task. No frame bitmaps,
 * per-frame allocations, floating point or platform-specific drawing. */
typedef struct {
    vui_dot_t dots[VIBE_HOME_DOT_COUNT];
    vui_dot_t ripple_from[VIBE_HOME_DOT_COUNT];
    uint64_t entered_ms, ripple_ms;
    int touch_x, touch_y;
    bool ripple;
} vibe_home_motion_t;

void vibe_home_motion_enter(vibe_home_motion_t *motion, uint64_t now);
void vibe_home_motion_sample(vibe_home_motion_t *motion, uint64_t now);
void vibe_home_motion_tap(vibe_home_motion_t *motion, uint64_t now, int x, int y);
