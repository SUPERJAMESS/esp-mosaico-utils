// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stddef.h>
#include "app_game.h"

int main(void)
{
    app_game_handle_t a = NULL, b = NULL;
    app_game_config_t config = {480, 320};
    assert(app_game_create(NULL, &a) == APP_GAME_INVALID_ARGUMENT && a == NULL);
    assert(app_game_create(&config, NULL) == APP_GAME_INVALID_ARGUMENT);
    config.width = 0;
    assert(app_game_create(&config, &a) == APP_GAME_INVALID_ARGUMENT);
    config.width = 480;
    assert(app_game_create(&config, &a) == APP_GAME_OK);
    assert(app_game_create(&config, &b) == APP_GAME_OK);
    config.width = 1; /* The model owns a copy, not this caller storage. */
    app_game_set_pointer(a, 1000, -10, true);
    app_game_update(a);
    app_game_snapshot_t snapshot = app_game_read(a);
    assert(snapshot.pointer_x == 479 && snapshot.pointer_y == 0);
    assert(snapshot.pointer_down && snapshot.tick == 1);
    snapshot.tick = 999;
    assert(app_game_read(a).tick == 1 && app_game_read(b).tick == 0);
    app_game_set_paused(a, true);
    app_game_update(a);
    assert(app_game_read(a).tick == 1);
    app_game_set_pointer(a, 5, 6, false);
    assert(app_game_read(a).pointer_x == 5);
    app_game_set_paused(a, false);
    app_game_update(a);
    assert(app_game_read(a).tick == 2);
    app_game_reset(a);
    snapshot = app_game_read(a);
    assert(snapshot.tick == 0 && !snapshot.paused && !snapshot.pointer_down);
    app_game_set_pointer(a, 1000, 1000, true);
    assert(app_game_read(a).pointer_y == 319);
    app_game_destroy(&a);
    assert(a == NULL);
    app_game_destroy(&a);
    app_game_destroy(NULL);
    app_game_update(NULL);
    app_game_reset(NULL);
    app_game_set_paused(NULL, true);
    app_game_set_pointer(NULL, 1, 2, true);
    assert(app_game_read(NULL).tick == 0);
    app_game_destroy(&b);
    return 0;
}
