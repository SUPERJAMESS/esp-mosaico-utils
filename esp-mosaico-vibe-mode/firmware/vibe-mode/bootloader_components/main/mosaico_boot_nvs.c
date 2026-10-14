// SPDX-License-Identifier: Apache-2.0
#include "mosaico_boot_nvs.h"
#include "mosaico_recovery_contract.h"
#include "nvs_bootloader_private.h"
#include "esp_log.h"
#include <string.h>

/* The pinned SDK's general bootloader reader rejects multiple ACTIVE pages,
 * while its runtime PageManager can load that state. Keep its CRC, entry-state
 * and span validation, but read only the product's namespace and U32 key.
 * Match runtime Storage's ascending page-sequence/entry lookup order, including
 * duplicate namespace records. Ambiguous page sequences/GC select Vibe Mode.
 * This file intentionally depends on the pinned SDK's private NVS reader API.
 */
static esp_err_t read_page(const esp_partition_t *partition, size_t page,
                           nvs_bootloader_page_header_t *header)
{
    esp_err_t err = nvs_bootloader_read_page_header(partition, page, header);
    if (err == ESP_ERR_NVS_INVALID_STATE) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (header->page_state != NVS_CONST_PAGE_STATE_ACTIVE &&
        header->page_state != NVS_CONST_PAGE_STATE_FULL &&
        header->page_state != NVS_CONST_PAGE_STATE_FREEING)
        return ESP_ERR_NVS_NOT_FOUND;
    return ESP_OK;
}

static esp_err_t scan_entries(const esp_partition_t *partition, bool skip_active,
                              uint8_t *namespace_index, uint32_t *intent,
                              bool read_value)
{
    bool found = false;
    uint32_t first_sequence = 0;
    size_t first_page = 0;
    for (size_t page = 0; page < partition->size / NVS_CONST_PAGE_SIZE; ++page) {
        nvs_bootloader_page_header_t header;
        esp_err_t err = read_page(partition, page, &header);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK) return err;
        if (skip_active && header.page_state == NVS_CONST_PAGE_STATE_ACTIVE) continue;
        _Alignas(uint32_t) nvs_bootloader_page_entry_states_t states;
        err = nvs_bootloader_read_page_entry_states(partition, page, &states);
        if (err != ESP_OK) return err;
        uint8_t next = 0;
        nvs_bootloader_single_entry_t item;
        while ((err = nvs_bootloader_read_next_single_entry_item(
                    partition, page, &states, &next, &item)) == ESP_OK) {
            if (!read_value) {
                if (item.namespace_index != 0 || item.data_type != NVS_TYPE_U8 ||
                    strncmp(item.key, MOSAICO_BOOT_NAMESPACE, sizeof(item.key)) != 0)
                    continue;
                const uint8_t index = item.data.primitive_type.data[0];
                if (index == 0 || index == UINT8_MAX)
                    return ESP_ERR_INVALID_STATE;
                if (!found || header.sequence_number < first_sequence)
                    *namespace_index = index;
            } else {
                if (item.namespace_index != *namespace_index ||
                    strncmp(item.key, MOSAICO_BOOT_INTENT_KEY, sizeof(item.key)) != 0)
                    continue;
                if (item.data_type != NVS_TYPE_U32) return ESP_ERR_NVS_TYPE_MISMATCH;
                uint32_t value;
                memcpy(&value, item.data.primitive_type.data, sizeof(value));
                if (!found || header.sequence_number < first_sequence)
                    *intent = value;
            }
            if (found && header.sequence_number == first_sequence && page != first_page) {
                ESP_LOGE("boot_nvs", "Duplicate NVS page sequence for boot intent");
                return ESP_ERR_INVALID_STATE;
            }
            if (!found || header.sequence_number < first_sequence) {
                first_sequence = header.sequence_number;
                first_page = page;
            }
            found = true;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    }
    return found ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t mosaico_boot_read_intent(uint32_t *intent)
{
    if (intent == NULL) return ESP_ERR_INVALID_ARG;
    *intent = MOSAICO_BOOT_INTENT_NONE;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
        MOSAICO_SYSMETA_PARTITION);
    if (partition == NULL) return ESP_ERR_NVS_PART_NOT_FOUND;
    size_t active = 0, freeing = 0;
    for (size_t page = 0; page < partition->size / NVS_CONST_PAGE_SIZE; ++page) {
        nvs_bootloader_page_header_t header;
        esp_err_t err = read_page(partition, page, &header);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK) return err;
        active += header.page_state == NVS_CONST_PAGE_STATE_ACTIVE;
        freeing += header.page_state == NVS_CONST_PAGE_STATE_FREEING;
    }
    if (freeing > 1 || (freeing && active > 1)) return ESP_ERR_INVALID_STATE;
    uint8_t namespace_index = 0;
    esp_err_t err = scan_entries(partition, freeing != 0, &namespace_index, intent, false);
    if (err == ESP_OK)
        err = scan_entries(partition, freeing != 0, &namespace_index, intent, true);
    return err;
}
