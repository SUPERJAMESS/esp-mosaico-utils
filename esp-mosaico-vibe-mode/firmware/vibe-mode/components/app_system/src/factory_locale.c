// SPDX-License-Identifier: Apache-2.0
#include "factory_locale.h"
#include "factory_system_metadata.h"
#include "nvs.h"
#include <stdatomic.h>
static atomic_uint language;
void factory_locale_init(void) {
    nvs_handle_t handle;
    uint8_t saved = 0;
    if (nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "settings", NVS_READONLY,
                                &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "language", &saved) != ESP_OK || saved > 1)
            saved = 0;
        nvs_close(handle);
    }
    atomic_store(&language, saved);
}
unsigned factory_locale_get(void) { return atomic_load(&language); }
void factory_locale_use(unsigned selected) { if (selected <= 1) atomic_store(&language, selected); }
esp_err_t factory_locale_set(unsigned selected) {
    if (selected > 1)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(FACTORY_SYSTEM_METADATA_PARTITION, "settings",
                                            NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(handle, "language", selected);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK)
        atomic_store(&language, selected);
    return err;
}
