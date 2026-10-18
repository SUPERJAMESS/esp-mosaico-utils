// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "vui.h"
#include "vibe_motion.h"
#define VIBE_HOME_DOTS_X 280
#define VIBE_HOME_DOTS_Y 64
#define VIBE_HOME_DOTS_SIZE 172
/* Zero-initialize before first enter. Render-task owned; no allocations or
 * platform/setting side effects. Subsequent enters resume the paused story. */
typedef struct {
    vibe_companion_t companion;
    vui_matrix_t frame;
    uint64_t sample_ms;
    bool initialized;
} vibe_home_motion_t;
void vibe_home_motion_enter(vibe_home_motion_t *motion, uint64_t now);
void vibe_home_motion_sample(vibe_home_motion_t *motion, uint64_t now);
void vibe_home_motion_input(vibe_home_motion_t *motion, int x, int y, bool pressed, bool cancelled);
