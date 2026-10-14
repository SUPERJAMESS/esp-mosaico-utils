// SPDX-License-Identifier: Apache-2.0
#include "mosaico_boot.h"
#include "mosaico_recovery_contract.h"

#include "esp_image_format.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "mosaico_boot";

static esp_err_t consume_bootstrap(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    if (partition == NULL || partition->size != MOSAICO_OTADATA_BYTES)
        return ESP_ERR_NOT_FOUND;
    char marker[MOSAICO_BOOTSTRAP_BYTES];
    esp_err_t err = esp_partition_read(partition, MOSAICO_BOOTSTRAP_OFFSET,
                                      marker, sizeof(marker));
    if (err != ESP_OK) return err;
    if (memcmp(marker, MOSAICO_BOOTSTRAP_MARKER, sizeof(marker)) != 0)
        return ESP_OK;
    /* The install image has no OTA sequence in this sector. Erasing it also
     * works with encrypted otadata, unlike rewriting individual marker bits. */
    return esp_partition_erase_range(partition, 0, MOSAICO_FLASH_SECTOR_BYTES);
}

static const esp_partition_t *recovery_partition(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST,
        MOSAICO_RECOVERY_PARTITION);
    return partition != NULL && partition->address == MOSAICO_RECOVERY_ADDRESS &&
        partition->size == MOSAICO_RECOVERY_BYTES ? partition : NULL;
}

static esp_err_t write_intent(uint32_t intent)
{
    esp_err_t err = nvs_flash_init_partition(MOSAICO_SYSMETA_PARTITION);
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open_from_partition(MOSAICO_SYSMETA_PARTITION,
        MOSAICO_BOOT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint32_t current = 0;
    const esp_err_t read_err = nvs_get_u32(handle, MOSAICO_BOOT_INTENT_KEY, &current);
    if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) {
        err = read_err;
    } else if (current != intent) {
        err = nvs_set_u32(handle, MOSAICO_BOOT_INTENT_KEY, intent);
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t mosaico_boot_request_recovery(void)
{
    const esp_partition_t *partition = recovery_partition();
    if (partition == NULL) return ESP_ERR_NOT_FOUND;
    const esp_partition_pos_t position = {
        .offset = partition->address, .size = partition->size,
    };
    esp_image_metadata_t metadata = {0};
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Vibe Mode image verified; recording boot intent");
    err = write_intent(MOSAICO_BOOT_INTENT_RECOVERY);
    if (err == ESP_OK) ESP_LOGI(TAG, "Vibe Mode boot intent committed");
    return err;
}

esp_err_t mosaico_boot_recovery_requested(bool *requested)
{
    if (requested == NULL) return ESP_ERR_INVALID_ARG;
    *requested = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(MOSAICO_SYSMETA_PARTITION,
        MOSAICO_BOOT_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint32_t intent = 0;
    err = nvs_get_u32(handle, MOSAICO_BOOT_INTENT_KEY, &intent);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err == ESP_OK) *requested = intent == MOSAICO_BOOT_INTENT_RECOVERY;
    return err;
}

esp_err_t mosaico_boot_consume_request(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *recovery = recovery_partition();
    if (running == NULL || recovery == NULL || running->address != recovery->address)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = write_intent(MOSAICO_BOOT_INTENT_NONE);
    return err == ESP_OK ? consume_bootstrap() : err;
}

esp_err_t mosaico_boot_select(uint32_t address)
{
    const esp_partition_t *recovery = recovery_partition();
    if (recovery == NULL) return ESP_ERR_NOT_FOUND;
    if (address == recovery->address) return mosaico_boot_request_recovery();
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL && partition->address == address &&
            partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            err = esp_ota_set_boot_partition(partition);
            if (err == ESP_OK) err = write_intent(MOSAICO_BOOT_INTENT_NONE);
            break;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return err;
}

esp_err_t esp_iris_platform_select_recovery_target(uint32_t *address)
{
    if (address == NULL) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *partition = recovery_partition();
    if (partition == NULL) return ESP_ERR_NOT_FOUND;
    *address = partition->address;
    return ESP_OK;
}

esp_err_t esp_iris_platform_set_boot_target(uint32_t address)
{
    return mosaico_boot_select(address);
}
