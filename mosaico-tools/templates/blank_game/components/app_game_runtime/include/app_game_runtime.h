// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "raylib_lite_game_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the immutable Engine ABI v1 descriptor; never NULL, never free it.
 * Thread-safe, nonblocking and allocation-free.
 * Callback lifecycle, return values and timing: ../README.md. */
const raylib_lite_game_module_v1_t *app_game_runtime_module(void);

#ifdef __cplusplus
}
#endif
