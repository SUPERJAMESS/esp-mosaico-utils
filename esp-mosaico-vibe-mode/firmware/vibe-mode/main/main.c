// SPDX-License-Identifier: Apache-2.0

#include "esp_err.h"
#include "app_system_startup.h"
#include "app_iris_storage.h"
#include "app_iris_ui.h"
#include "factory_nand_update.h"
#include "factory_ideas.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "factory_network.h"
#include "iris_bridge.h"
#include "factory_recovery_control.h"
#include "factory_system_inventory.h"
#include "factory_system_metadata.h"
#include "factory_system_update.h"
#include "factory_ui.h"
#include "recovery_ota_support.h"
#include "nvs_flash.h"
#include "mosaico_boot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "factory";

void app_main(void)
{
    ESP_ERROR_CHECK(app_system_mute_audio());

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(factory_system_metadata_init());
    bool requested = false;
    const esp_err_t intent_err = mosaico_boot_recovery_requested(&requested);
    ESP_ERROR_CHECK(mosaico_boot_consume_request());
    const esp_err_t resume_err = iris_bridge_resume_boot();
    if (resume_err != ESP_OK) {
        ESP_LOGE(TAG, "Bridge boot selection failed; staying in Recovery: %s",
                 esp_err_to_name(resume_err));
    }

    /* Make the retained USB OTA writer reachable before display and network
     * initialization. Inventory and system-update providers must be
     * registered before esp_iris_start(); the screen backend may be attached
     * after the transport is running. */
    ESP_ERROR_CHECK(factory_system_inventory_register());
    ESP_ERROR_CHECK(factory_system_update_register());
    ESP_ERROR_CHECK(factory_nand_update_init());
    ESP_ERROR_CHECK(app_iris_storage_register());
    const app_iris_ui_t ui_commands = {
        .open_download = factory_ui_open_bridge,
        .touch = factory_ui_inject_touch,
    };
    ESP_ERROR_CHECK(factory_recovery_control_register(&ui_commands));
    recovery_ota_support_start();

    esp_err_t ui_err = factory_ui_start();
    if (ui_err != ESP_OK) {
        /* Keep the already-started USB writer available for a corrected
         * image, but do not announce a healthy Recovery with a broken UI. */
        ESP_LOGE(TAG, "Vibe Mode UI failed: %s; USB maintenance remains active",
                 esp_err_to_name(ui_err));
        return;
    }
    ESP_LOGI(TAG, "UI initialization stack headroom: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    const esp_err_t input_err = app_iris_input_register(&ui_commands);
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "Vibe Mode input failed: %s; USB maintenance remains active",
                 esp_err_to_name(input_err));
        return;
    }

    /* Initialize the Wi-Fi driver first, then reserve the lifetime Ideas HP
     * SRAM stack and queue its scan. Later UI jobs reuse this allocation. */
    const esp_err_t network_err = factory_network_start();
    if (network_err != ESP_OK) {
        ESP_LOGE(TAG, "Factory network unavailable; USB recovery remains active: %s",
                 esp_err_to_name(network_err));
    }
    const esp_err_t ideas_err = factory_ideas_init();
    if (ideas_err != ESP_OK) {
        ESP_LOGE(TAG, "Ideas initialization failed: %s; USB maintenance remains active",
                 esp_err_to_name(ideas_err));
        return;
    }

#if CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_AUTO_START
    if (CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_MANIFEST_PATH[0] != '\0') {
        const esp_err_t update_err =
            factory_system_update_start_nand(CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE_MANIFEST_PATH);
        if (update_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start configured NAND system update: %s",
                     esp_err_to_name(update_err));
        }
    }
#endif
    /* This is the Recovery acceptance boundary used by the host's closed-loop
     * self-update workflow.  esp_iris_mark_healthy() replays the event when a
     * USB session connects after this point. */
    app_system_log_boot_selection(requested, intent_err);
    ESP_ERROR_CHECK(esp_iris_mark_healthy());
    ESP_LOGI(TAG, "ESP-Mosaico Vibe Mode firmware is ready");
}
