// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "esp_err.h"
#include "vui.h"

/** Register before the renderer starts; retain its live shadow and coverage in PSRAM. */
esp_err_t iris_screen_mirror_init(void);

/** Attach the running UI. Capture waits until the first frame covers the panel. */
esp_err_t iris_screen_mirror_attach(vui_handle_t ui);
