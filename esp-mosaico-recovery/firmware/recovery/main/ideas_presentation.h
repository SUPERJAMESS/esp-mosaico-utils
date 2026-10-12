// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_gsp.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* Consume remote touches while the information card owns the display. */
bool ideas_presentation_input(int32_t x, int32_t y, bool pressed);
/* Worker-task only; the caller holds the NAND lease and verified the file.
 * Returns after the user closes the static localized information card. */
esp_err_t ideas_presentation_show(esp_gsp_handle_t hub, const char *path);
