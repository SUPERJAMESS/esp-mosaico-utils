#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t factory_ui_start(void);

/* Stable control entrypoint: open the six-digit Ideas download page. */
esp_err_t factory_ui_open_bridge(void);

#ifdef __cplusplus
}
#endif
