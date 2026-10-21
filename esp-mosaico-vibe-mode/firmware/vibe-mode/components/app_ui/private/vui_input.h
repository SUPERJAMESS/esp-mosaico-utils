// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "vui.h"

/* Render-task-only: the hit captured before the pointer observer runs. */
bool vui_pointer_target(vui_handle_t ui, vui_event_t *event);
/* Sticky for the entire contact, including a touch that stops a fling. */
bool vui_pointer_moved(vui_handle_t ui);
/* Finger velocity in logical pixels/second, using acquisition timestamps. */
int vui_pointer_velocity_x(vui_handle_t ui);
