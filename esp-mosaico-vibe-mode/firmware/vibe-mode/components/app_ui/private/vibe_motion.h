// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VIBE_MOTION_GRID 15
#define VIBE_MOTION_PIXELS 225
#define VIBE_MOTION_CYCLE_MS 12000U
#define VIBE_RIPPLE_MS 650U
#define VIBE_RIPPLE_LIMIT 2

/* Independent white/accent intensities, in [0, 1]. No allocation or UI state. */
typedef struct { float x, y; uint32_t age_ms; } vibe_ripple_t;
typedef enum { VIBE_USB_UNPLUGGED, VIBE_USB_WAITING, VIBE_USB_READY } vibe_usb_motion_t;
typedef enum { VIBE_STATUS_SUCCESS, VIBE_STATUS_FAILED, VIBE_STATUS_STORAGE,
               VIBE_STATUS_BATTERY, VIBE_STATUS_OFFLINE } vibe_status_motion_t;
typedef enum { VIBE_FACE_NEUTRAL, VIBE_FACE_SMILE, VIBE_FACE_HAPPY,
               VIBE_FACE_WINK, VIBE_FACE_SURPRISED, VIBE_FACE_SLEEPY } vibe_face_mood_t;
typedef struct {
    uint32_t intro, idle, look, pose, held, tap_gap;
    uint32_t pose_age, gaze_age, quiet, joy;
    float x, y, x0, y0, tap_x, tap_y;
    int gx, gy;
    vibe_face_mood_t mood;
    bool down, moved;
    float response_from[22];
    uint32_t response_age, return_age;
    int intent;
    bool hit, response_valid;
} vibe_companion_t;

/* Visible-time state only: no allocations, timers, storage or haptic side effects. */
void vibe_companion_init(vibe_companion_t *s);
void vibe_companion_tick(vibe_companion_t *s, uint32_t dt);
void vibe_companion_input(vibe_companion_t *s, float x, float y, bool pressed, bool cancelled);
void vibe_companion_frame(const vibe_companion_t *s, float *white, float *accent, bool reduced);

bool vibe_motion_inside(int n, int x, int y);
void vibe_motion_assembly(float *white, float *accent, uint32_t ms);
void vibe_motion_home(float *white, float *accent, uint32_t ms, bool reduced);
void vibe_motion_ripple(float *white, float *accent, const vibe_ripple_t *ripples,
                        unsigned count, const vibe_ripple_t *press);
void vibe_motion_usb(float *white, float *accent, vibe_usb_motion_t state, uint32_t ms, bool reduced);
void vibe_motion_status(float *white, float *accent, vibe_status_motion_t kind, uint32_t ms, bool reduced);
