#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start board display, screenshot adapter and render-owned controller.
 * Call once after Ideas/Iris initialization; returns startup/timeout errors. */
esp_err_t factory_ui_start(void);
/* Device-service task only; drains rendering before changing panel and touch. */
esp_err_t factory_ui_rotate(unsigned degrees);

/* Open the six-digit Ideas page from a worker task. ctx is unused. Bounded
 * queue/response wait; returns timeout or the render-thread operation result. */
esp_err_t factory_ui_open_bridge(void *ctx);
/* Iris-worker adapter: ctx is unused; bounds are checked before queued input.
 * UI must already be started. Returns argument, state, or GSP input errors. */
esp_err_t factory_ui_inject_touch(void *ctx, int16_t x, int16_t y, bool pressed);

#ifdef __cplusplus
}
#endif
