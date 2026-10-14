// SPDX-License-Identifier: Apache-2.0
#include "app_system_startup.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
static const char *TAG = "factory";

void app_system_log_boot_selection(bool requested, esp_err_t intent_err)
{
    const esp_partition_t *selected = esp_ota_get_boot_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_err =
        selected == NULL ? ESP_ERR_NOT_FOUND : esp_ota_get_state_partition(selected, &state);
    ESP_LOGI(TAG, "Vibe boot: AI=%d intent=%d (%s) selected=%s state=%d (%s)",
             gpio_get_level(BSP_BUTTON_AI_GPIO), requested, esp_err_to_name(intent_err),
             selected == NULL ? "none" : selected->label, (int)state, esp_err_to_name(state_err));
}

esp_err_t app_system_mute_audio(void)
{
    /* Software restart can retain the application's amplifier state. Keep
     * the speaker physically muted before starting any Recovery services. */
    const gpio_config_t pa_config = {
        .pin_bit_mask = 1ULL << BSP_AUDIO_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AUDIO_PA_CTRL, 0), TAG, "mute PA");
    ESP_RETURN_ON_ERROR(gpio_config(&pa_config), TAG, "configure PA");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(BSP_AUDIO_PA_CTRL), TAG, "release PA hold");

    return ESP_OK;
}
