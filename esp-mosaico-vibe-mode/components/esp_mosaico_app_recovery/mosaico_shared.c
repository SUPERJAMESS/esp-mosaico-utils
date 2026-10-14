// SPDX-License-Identifier: Apache-2.0
#include "mosaico_shared.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

esp_err_t mosaico_shared_get(const char *key, char *value, size_t capacity)
{
    if (!key || !*key || strlen(key) > 15 ||
        strspn(key, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(key) ||
        !value || !capacity)
        return ESP_ERR_INVALID_ARG;
    value[0] = 0;
    esp_err_t err = nvs_flash_init_partition("sysmeta");
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open_from_partition("sysmeta", "shared", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, key, value, &capacity);
    nvs_close(handle);
    return err;
}
