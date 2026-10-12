// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_gsp.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef esp_err_t (*ideas_presentation_decide_fn)(uint32_t request_id, bool download);
typedef struct {
    uint32_t request_id;
    const char *summary;
    bool already_downloaded;
    ideas_presentation_decide_fn decide;
} ideas_presentation_options_t;
/* All touches are consumed while the card owns the display. */
bool ideas_presentation_input(int32_t x, int32_t y, bool pressed);
/* Worker only. Borrows aligned, verified bytes until close. Opening does not
 * wait for user input; enable only after the owner publishes its ready state. */
esp_err_t ideas_presentation_open(esp_gsp_handle_t hub, const void *data, size_t size,
                                  const ideas_presentation_options_t *options);
void ideas_presentation_enable(void);
void ideas_presentation_close(void);
/* Existing offline View info; returns after Back. */
esp_err_t ideas_presentation_show(esp_gsp_handle_t hub, const char *path);
