#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* First startup operation: clear retained amplifier state before services run.
 * Returns a GPIO error; the entrypoint decides the fatal startup policy. */
esp_err_t app_system_mute_audio(void);
/* Log the consumed request and selected partition after service startup.
 * requested/intent_err are the result captured before consuming the intent. */
void app_system_log_boot_selection(bool requested, esp_err_t intent_err);
