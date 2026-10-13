// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_gsp.h"
bool gsp_hello_event_is_say_hello(const esp_gsp_event_t *);
esp_gsp_err_t gsp_hello_count_set_text(esp_gsp_handle_t, const char *);
esp_gsp_err_t gsp_hello_celebration_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_0_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_1_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_2_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_3_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_4_set_visible(esp_gsp_handle_t, bool);
esp_gsp_err_t gsp_hello_confetti_5_set_visible(esp_gsp_handle_t, bool);
