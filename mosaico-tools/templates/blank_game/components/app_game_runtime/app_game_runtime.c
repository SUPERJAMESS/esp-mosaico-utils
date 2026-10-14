// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "include/app_game_runtime.h"
/* Engine Host accepts source lists, not component include paths. Reference only
 * the model's public header so these translation units work on both targets. */
#include "../app_game/include/app_game.h"
#include "private/game_config.h"
#include "raylib_lite_raylib.h"

typedef struct {
    app_game_handle_t game;
} host_state_t;

static int initialize(void *value, const char *asset_root)
{
    (void)asset_root;
    host_state_t *state = value;
    const app_game_config_t config = {GAME_WIDTH, GAME_HEIGHT};
    if (app_game_create(&config, &state->game) != APP_GAME_OK) {
        return -1;
    }
    InitWindow(GAME_WIDTH, GAME_HEIGHT, GAME_NAME);
    SetTargetFPS(GAME_TICK_HZ);
    return 0;
}

static void shutdown(void *value)
{
    host_state_t *state = value;
    app_game_destroy(&state->game);
}

static void input(void *value, const raylib_lite_game_input_v1_t *event)
{
    host_state_t *state = value;
    if (!event) {
        return;
    }
    if (event->type == RAYLIB_LITE_GAME_INPUT_POINTER && event->track_id == 0) {
        app_game_set_pointer(state->game, event->x, event->y, event->pressed);
    } else if (event->type == RAYLIB_LITE_GAME_INPUT_CONTROL) {
        switch (event->code) {
        case RAYLIB_LITE_GAME_CONTROL_PAUSE: app_game_set_paused(state->game, true); break;
        case RAYLIB_LITE_GAME_CONTROL_RESUME: app_game_set_paused(state->game, false); break;
        case RAYLIB_LITE_GAME_CONTROL_RESET: app_game_reset(state->game); break;
        default: break;
        }
    } else if (event->type == RAYLIB_LITE_GAME_INPUT_ACTION && event->pressed) {
        if (event->code == GAME_ACTION_RESET) {
            app_game_reset(state->game);
        }
        if (event->code == GAME_ACTION_PAUSE) {
            app_game_set_paused(state->game, !app_game_read(state->game).paused);
        }
    }
}

static void update(void *value)
{
    app_game_update(((host_state_t *)value)->game);
}

static int render(void *value)
{
    if (!((host_state_t *)value)->game) {
        return -1;
    }
    BeginDrawing();
    if (!raylib_lite_raylib_frame_available()) {
        EndDrawing();
        return -1;
    }
    ClearBackground(BLACK);
    /* Read a model snapshot here when adding gameplay rendering. Keep hardware
     * and drawing calls out of app_game so its rules remain independently testable. */
    EndDrawing();
    return 0;
}

static uint32_t state_hash(const void *value)
{
    const app_game_snapshot_t state = app_game_read(((const host_state_t *)value)->game);
    /* Hash named fields, avoiding platform-dependent struct padding. */
    uint32_t hash = state.tick;
    hash = hash * 31U + (uint32_t)state.pointer_x;
    hash = hash * 31U + (uint32_t)state.pointer_y;
    hash = hash * 31U + state.pointer_down;
    return hash * 31U + state.paused;
}

static int state_json(const void *value, char *output, size_t capacity)
{
    if (!output || !capacity) {
        return -1;
    }
    const app_game_snapshot_t state = app_game_read(((const host_state_t *)value)->game);
    int size = snprintf(output, capacity,
                        "{\"phase\":\"%s\",\"tick\":%lu,\"pointer_x\":%ld,\"pointer_y\":%ld,"
                        "\"pointer_down\":%s,\"state_hash\":\"%08lx\"}",
                        state.paused ? "paused" : "running", (unsigned long)state.tick,
                        (long)state.pointer_x, (long)state.pointer_y,
                        state.pointer_down ? "true" : "false", (unsigned long)state_hash(value));
    return size < 0 || (size_t)size >= capacity ? -1 : size;
}

static const raylib_lite_game_module_v1_t s_module = {
    .descriptor = {
        RAYLIB_LITE_GAME_MODULE_ABI_V1, GAME_NAME, GAME_NAME,
        GAME_WIDTH, GAME_HEIGHT, GAME_TICK_HZ, 1
    },
    .state_size = sizeof(host_state_t),
    .initialize = initialize, .shutdown = shutdown, .input = input,
    .update = update, .render = render,
    .state_hash = state_hash, .state_json = state_json,
};

const raylib_lite_game_module_v1_t *app_game_runtime_module(void)
{
    return &s_module;
}
