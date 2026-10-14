#pragma once
#include "esp_err.h"

/* Explicit entry promotes an existing prefetched session without registering.
 * Start or activate the configured Bridge session. Copies local configuration;
 * returns start/policy error. Task context; does not block for network readiness. */
esp_err_t factory_bridge_open(void);
/* Start registration-only work without polling or executing remote jobs.
 * Returns start/policy error; the Bridge worker waits for network readiness. */
esp_err_t factory_bridge_prefetch(void);
