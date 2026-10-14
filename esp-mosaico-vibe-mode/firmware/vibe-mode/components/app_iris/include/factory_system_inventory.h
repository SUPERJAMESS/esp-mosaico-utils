#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the retained read-only inventory provider before Iris starts.
 * Disabled inventory is a successful no-op; otherwise returns registration error. */
esp_err_t factory_system_inventory_register(void);

#ifdef __cplusplus
}
#endif
