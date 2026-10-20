// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "vui.h"

/* One reusable RGB565 snapshot; only the viewport moves. Render-task owned. */
vui_err_t vui_transition_begin(vui_handle_t ui, vui_area_t viewport);
void vui_transition_update(vui_handle_t ui, int offset, bool incoming);
void vui_transition_end(vui_handle_t ui);
