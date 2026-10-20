// SPDX-License-Identifier: Apache-2.0
#include "vibe_home_motion.h"
#include <string.h>
static void project(vibe_home_motion_t *m)
{
    float white[VIBE_MOTION_PIXELS], accent[VIBE_MOTION_PIXELS];
    vibe_companion_frame(&m->companion, white, accent, false);
    for (unsigned i = 0; i < VIBE_MOTION_PIXELS; ++i) {
        m->frame.white[i] = white[i] > .5f ? 255 : 0;
        m->frame.accent[i] = accent[i] > .5f ? 255 : 0;
    }
}
void vibe_home_motion_enter(vibe_home_motion_t *m, uint64_t now)
{
    if (!m->initialized) {
        memset(m, 0, sizeof(*m));
        vibe_companion_init(&m->companion);
        m->initialized = true;
    }
    m->sample_ms = now;
    project(m);
}
void vibe_home_motion_sample(vibe_home_motion_t *m, uint64_t now)
{
    uint64_t elapsed = now >= m->sample_ms ? now - m->sample_ms : 0;
    m->sample_ms = now;
    /* Only visible time advances the story; a stalled frame cannot skip a scene. */
    vibe_companion_tick(&m->companion, elapsed > 100 ? 100 : (uint32_t)elapsed);
    project(m);
}
void vibe_home_motion_input(vibe_home_motion_t *m, int x, int y, bool pressed, bool cancelled)
{
    vibe_companion_input(&m->companion, (x - 11) / 10.0f, (y - 11) / 10.0f, pressed, cancelled);
    project(m);
}
