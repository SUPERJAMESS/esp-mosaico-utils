// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "vibe_ui.h"

/* UI/state and service context must outlive the render timer; the service
 * table is copied. Startup and bridge requests have bounded waits even when
 * the render task stops processing timers. */
esp_err_t factory_ui_dispatch_start(vui_handle_t ui, const vibe_services_t *services);
esp_err_t factory_ui_dispatch_open_bridge(void);
