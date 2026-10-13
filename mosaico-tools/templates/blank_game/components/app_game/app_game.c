// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
#include "include/app_game.h"

struct app_game {
    app_game_config_t config;
    app_game_snapshot_t state;
};

app_game_err_t app_game_create(const app_game_config_t *config, app_game_handle_t *ret_handle)
{
    if (!ret_handle) {
        return APP_GAME_INVALID_ARGUMENT;
    }
    *ret_handle = NULL;
    if (!config || !config->width || !config->height) {
        return APP_GAME_INVALID_ARGUMENT;
    }
    app_game_handle_t game = calloc(1, sizeof(*game));
    if (!game) {
        return APP_GAME_NO_MEMORY;
    }
    game->config = *config;
    *ret_handle = game;
    return APP_GAME_OK;
}

void app_game_destroy(app_game_handle_t *handle)
{
    if (handle) {
        free(*handle);
        *handle = NULL;
    }
}

void app_game_reset(app_game_handle_t game)
{
    if (game) {
        game->state = (app_game_snapshot_t) {
            0
        };
    }
}

void app_game_set_paused(app_game_handle_t game, bool paused)
{
    if (game) {
        game->state.paused = paused;
    }
}

void app_game_set_pointer(app_game_handle_t game, int32_t x, int32_t y, bool pressed)
{
    if (!game) {
        return;
    }
    game->state.pointer_x = x < 0 ? 0 : (x >= game->config.width ? game->config.width - 1 : x);
    game->state.pointer_y = y < 0 ? 0 : (y >= game->config.height ? game->config.height - 1 : y);
    game->state.pointer_down = pressed;
    /* Add touch-driven game logic here. */
}

void app_game_update(app_game_handle_t game)
{
    if (!game || game->state.paused) {
        return;
    }
    ++game->state.tick;
    /* Add fixed-step game logic here. */
}

app_game_snapshot_t app_game_read(app_game_handle_t game)
{
    return game ? game->state : (app_game_snapshot_t) {
        0
    };
}
