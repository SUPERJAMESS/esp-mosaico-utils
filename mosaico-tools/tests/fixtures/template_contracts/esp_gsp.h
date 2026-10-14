// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESP_GSP_OK, ESP_GSP_ERR_INVALID_ARG, ESP_GSP_ERR_NO_MEM,
    ESP_GSP_ERR_INVALID_STATE
} esp_gsp_err_t;
typedef struct fake_ui *esp_gsp_handle_t;
typedef struct { bool hello; } esp_gsp_event_t;
typedef void (*esp_gsp_event_cb_t)(esp_gsp_handle_t, const esp_gsp_event_t *, void *);
typedef void (*esp_gsp_timer_cb_t)(esp_gsp_handle_t, void *);
esp_gsp_err_t esp_gsp_on_event(esp_gsp_handle_t, esp_gsp_event_cb_t, void *);
void *esp_gsp_timer_create(esp_gsp_handle_t, uint32_t, esp_gsp_timer_cb_t, void *);
esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t, void *);
