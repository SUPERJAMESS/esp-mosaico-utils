// SPDX-License-Identifier: Apache-2.0
#include "vibe_home_motion.h"
#include <stdlib.h>
#include <string.h>

enum { REST, INHALE, SCATTER, SETTLE, SCAN };
typedef struct {
    unsigned ms;
    unsigned pose;
    int band;
} keyframe_t;

static const keyframe_t entrance[] = {
    {0, SCATTER, 0}, {420, SETTLE, 0}, {1000, REST, 0},
};
/* Three quiet breaths followed by one diagonal scan. Both ends use REST,
 * including the join from the entrance, so looping never resets the shape. */
static const keyframe_t idle[] = {
    {0, REST, 0}, {1800, INHALE, 0}, {3600, REST, 0},
    {5400, INHALE, 0}, {7200, REST, 0}, {9000, INHALE, 0},
    {10800, REST, 0}, {11000, SCAN, 0}, {11200, SCAN, 2},
    {11400, SCAN, 4}, {11600, SCAN, 6}, {11800, SCAN, 8},
    {12000, SCAN, 10}, {12200, SCAN, 12}, {12400, REST, 0},
};
static const unsigned ripple_times[] = {0, 120, 270, 420, 570, 720, 900, 1100};
static const int ripple_radii[] = {-24, 0, 24, 48, 72, 96, 132, 180};

static unsigned smooth(unsigned elapsed, unsigned duration)
{
    unsigned t = elapsed >= duration ? 256 : elapsed * 256 / duration;
    return t * t * (768 - 2 * t) / 65536;
}
static int mix(int a, int b, unsigned t)
{
    return a + (b - a) * (int)t / 256;
}
static vui_dot_t interpolate(vui_dot_t a, vui_dot_t b, unsigned t)
{
    return (vui_dot_t){
        .x = mix(a.x, b.x, t), .y = mix(a.y, b.y, t),
        .radius = mix(a.radius, b.radius, t),
        .red = mix(a.red, b.red, t), .green = mix(a.green, b.green, t),
        .blue = mix(a.blue, b.blue, t),
        .brightness = mix(a.brightness, b.brightness, t),
    };
}
static vui_dot_t pose(unsigned i, const keyframe_t *key)
{
    static const char shape[7][8] = {
        "...W...", "..WWW..", ".W...W.", "W..O..W",
        ".W...W.", "..WWW..", "...W...",
    };
    int row = i / 7, col = i % 7;
    char role = shape[row][col];
    vui_dot_t dot = {
        .x = 960 + (col - 3) * 230, .y = 960 + (row - 3) * 230,
        .radius = role == '.' ? 64 : 88,
        .red = role == 'O' ? 255 : 245,
        .green = role == 'O' ? 76 : 245,
        .blue = role == 'O' ? 1 : 245,
        .brightness = role == '.' ? 26 : role == 'O' ? 220 : 190,
    };
    if (key->pose == SCATTER) {
        unsigned seed = (i * 17 + 11) % VIBE_HOME_DOT_COUNT;
        dot.x = 960 + ((int)(seed % 7) - 3) * 230 + ((int)(i % 3) - 1) * 48;
        dot.y = 960 + ((int)(seed / 7) - 3) * 230 + ((int)(i % 5) - 2) * 24;
        dot.radius = 24 + i % 3 * 8;
        dot.brightness = 70 + i % 4 * 30;
    } else if (key->pose == INHALE || key->pose == SETTLE) {
        int expansion = key->pose == SETTLE ? 20 : 9;
        dot.x += (col - 3) * expansion;
        dot.y += (row - 3) * expansion;
        dot.radius += role == '.' ? 4 : 12;
        dot.brightness = role == '.' ? 34 : 255;
    } else if (key->pose == SCAN) {
        int distance = abs(row + col - key->band);
        unsigned strength = distance >= 2 ? 0 : distance == 1 ? 128 : 256;
        vui_dot_t lit = dot;
        lit.red = 255; lit.green = 76; lit.blue = 1;
        lit.radius += 12; lit.brightness = 255;
        dot = interpolate(dot, lit, strength);
    }
    return dot;
}
static vui_dot_t timeline(unsigned i, uint64_t elapsed)
{
    const keyframe_t *keys = entrance;
    size_t count = sizeof(entrance) / sizeof(*entrance);
    if (elapsed >= 1000) {
        elapsed = (elapsed - 1000) % 12400;
        keys = idle;
        count = sizeof(idle) / sizeof(*idle);
    }
    size_t k = 1;
    while (k + 1 < count && elapsed >= keys[k].ms) ++k;
    unsigned t = smooth((unsigned)elapsed - keys[k - 1].ms, keys[k].ms - keys[k - 1].ms);
    return interpolate(pose(i, &keys[k - 1]), pose(i, &keys[k]), t);
}
static unsigned distance(unsigned squared)
{
    /* Integer square root; used only for 49 points, never in the pixel loop. */
    unsigned result = 0, bit = 1U << 30;
    while (bit > squared) bit >>= 2;
    while (bit) {
        if (squared >= result + bit) {
            squared -= result + bit;
            result = (result >> 1) + bit;
        } else result >>= 1;
        bit >>= 2;
    }
    return result;
}
static vui_dot_t ripple_pose(vibe_home_motion_t *m, unsigned i, unsigned k,
                             vui_dot_t baseline)
{
    if (!k) return m->ripple_from[i];
    if (k == 7) return baseline;
    /* Fade the interrupted pose into the moving idle timeline. Retriggering
     * starts from the currently displayed dots, without a discontinuity. */
    vui_dot_t dot = interpolate(m->ripple_from[i], baseline,
                                smooth(ripple_times[k], 1100));
    int dx = dot.x / 16 - m->touch_x, dy = dot.y / 16 - m->touch_y;
    unsigned dist = distance((unsigned)(dx * dx + dy * dy));
    int band = abs((int)dist - ripple_radii[k]);
    if (band >= 24) return dot;
    unsigned strength = (24 - band) * 256 / 24;
    vui_dot_t lit = dot;
    lit.red = 255; lit.green = 76; lit.blue = 1;
    lit.brightness = 255; lit.radius += 18;
    if (dist) {
        lit.x += dx * 32 / (int)dist;
        lit.y += dy * 32 / (int)dist;
    }
    return interpolate(dot, lit, strength);
}
void vibe_home_motion_sample(vibe_home_motion_t *m, uint64_t now)
{
    uint64_t elapsed = now >= m->entered_ms ? now - m->entered_ms : 0;
    uint64_t ripple_elapsed = now >= m->ripple_ms ? now - m->ripple_ms : 0;
    if (ripple_elapsed >= 1100) m->ripple = false;
    unsigned k = 1;
    if (m->ripple)
        while (k < 7 && ripple_elapsed >= ripple_times[k]) ++k;
    for (unsigned i = 0; i < VIBE_HOME_DOT_COUNT; ++i) {
        vui_dot_t dot = timeline(i, elapsed);
        if (m->ripple) {
            unsigned t = smooth((unsigned)ripple_elapsed - ripple_times[k - 1],
                                ripple_times[k] - ripple_times[k - 1]);
            dot = interpolate(ripple_pose(m, i, k - 1, dot),
                              ripple_pose(m, i, k, dot), t);
        }
        m->dots[i] = dot;
    }
}
void vibe_home_motion_enter(vibe_home_motion_t *m, uint64_t now)
{
    memset(m, 0, sizeof(*m));
    m->entered_ms = now;
    vibe_home_motion_sample(m, now);
}
void vibe_home_motion_tap(vibe_home_motion_t *m, uint64_t now, int x, int y)
{
    if (x < 0 || y < 0 || x >= VIBE_HOME_DOTS_SIZE || y >= VIBE_HOME_DOTS_SIZE)
        return;
    memcpy(m->ripple_from, m->dots, sizeof(m->dots));
    m->ripple_ms = now;
    m->touch_x = x; m->touch_y = y;
    m->ripple = true;
}
