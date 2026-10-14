// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

esp_err_t bridge_save_boot(const cJSON *plan)
{
    const char *target = bridge_str(plan, "boot_partition");
    if (!*target || !strcmp(target, "vibe_mode"))
        return ESP_OK;
    if (strlen(target) > 16)
        return ESP_ERR_INVALID_ARG;
    boot_record_t rec = {.version = 2};
    strlcpy(rec.target, target, sizeof(rec.target));
    strlcpy(rec.table_hash, bridge_str(plan, "target_table_sha256"), sizeof(rec.table_hash));
    nvs_handle_t n;
    esp_err_t err = nvs_open_from_partition("sysmeta", "iris_bridge_v2", NVS_READWRITE, &n);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(n, "pending_boot", &rec, sizeof(rec));
    if (err == ESP_OK)
        err = nvs_commit(n);
    nvs_close(n);
    return err;
}

esp_err_t iris_bridge_resume_boot(void)
{
    if (psa_crypto_init() != PSA_SUCCESS)
        return ESP_FAIL;
    nvs_handle_t n;
    esp_err_t err = nvs_open_from_partition("sysmeta", "iris_bridge_v2", NVS_READWRITE, &n);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (err != ESP_OK)
        return err;
    boot_record_t rec;
    size_t size = sizeof(rec);
    err = nvs_get_blob(n, "pending_boot", &rec, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(n);
        return ESP_OK;
    }
    const esp_err_t read_error = err;
    /* Consume malformed records too, before any boot selection. */
    err = nvs_erase_key(n, "pending_boot");
    if (err == ESP_OK)
        err = nvs_commit(n);
    nvs_close(n);
    if (err != ESP_OK)
        return err;
    if (read_error != ESP_OK)
        return read_error;
    char hash[65];
    if (size != sizeof(rec) || rec.version != 2 || rec.target[16] || rec.table_hash[64] ||
        !bridge_flash_hash(0x8000, 4096, hash) || strcmp(hash, rec.table_hash))
        return ESP_ERR_INVALID_VERSION;
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, rec.target);
    if (!p || p->address < 0x200000 || p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
        p->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15)
        return ESP_ERR_INVALID_ARG;
    err = mosaico_boot_select(p->address);
    if (err == ESP_OK)
        esp_restart();
    return err;
}
