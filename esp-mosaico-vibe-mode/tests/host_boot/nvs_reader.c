// SPDX-License-Identifier: Apache-2.0
#include "nvs_reader_sdk.h"
#include READER_SOURCE

static void item(size_t page, uint8_t ns, const char *key, uint8_t type, uint32_t value)
{
    nvs_bootloader_single_entry_t *out = &pages[page].items[pages[page].count++];
    out->namespace_index = ns;
    out->data_type = type;
    strcpy(out->key, key);
    memcpy(out->data.primitive_type.data, &value, sizeof(value));
}

static void setup(void)
{
    memset(pages, 0, sizeof(pages));
    for (size_t page = 0; page < 3; ++page) pages[page].sequence = page;
    pages[0].state = pages[2].state = NVS_CONST_PAGE_STATE_ACTIVE;
    pages[1].state = NVS_CONST_PAGE_STATE_FULL;
    item(0, 0, MOSAICO_BOOT_NAMESPACE, NVS_TYPE_U8, 3);
    item(1, 9, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, 999); /* Another namespace. */
    item(2, 3, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, MOSAICO_BOOT_INTENT_RECOVERY);
    partition_present = true;
    reads = fail_read = 0;
}

int main(void)
{
    uint32_t intent;
    setup();
    /* Two ACTIVE pages with the one live key are valid for runtime NVS. */
    assert(mosaico_boot_read_intent(&intent) == ESP_OK);
    assert(intent == MOSAICO_BOOT_INTENT_RECOVERY);
    const int read_count = reads;
    for (int fail = 1; fail <= read_count; ++fail) {
        setup(); fail_read = fail;
        assert(mosaico_boot_read_intent(&intent) == ESP_FAIL);
    }
    setup();
    item(0, 3, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, MOSAICO_BOOT_INTENT_NONE);
    assert(mosaico_boot_read_intent(&intent) == ESP_OK && intent == MOSAICO_BOOT_INTENT_NONE);
    pages[0].sequence = 3; /* Physical page order is not sequence order after GC. */
    assert(mosaico_boot_read_intent(&intent) == ESP_OK && intent == MOSAICO_BOOT_INTENT_RECOVERY);
    pages[0].sequence = pages[2].sequence;
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_INVALID_STATE);
    setup();
    item(0, 3, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, MOSAICO_BOOT_INTENT_RECOVERY);
    assert(mosaico_boot_read_intent(&intent) == ESP_OK); /* Identical GC copies. */
    setup();
    item(1, 0, MOSAICO_BOOT_NAMESPACE, NVS_TYPE_U8, 7);
    item(1, 7, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, MOSAICO_BOOT_INTENT_NONE);
    assert(mosaico_boot_read_intent(&intent) == ESP_OK && intent == MOSAICO_BOOT_INTENT_RECOVERY);
    pages[0].sequence = 3;
    assert(mosaico_boot_read_intent(&intent) == ESP_OK && intent == MOSAICO_BOOT_INTENT_NONE);
    setup();
    pages[2].items[0].data_type = NVS_TYPE_U8;
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_NVS_TYPE_MISMATCH);
    setup();
    pages[1].state = NVS_CONST_PAGE_STATE_FREEING;
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_INVALID_STATE);
    /* One interrupted GC source remains authoritative; ignore its ACTIVE copy. */
    setup();
    pages[0].state = NVS_CONST_PAGE_STATE_FULL;
    pages[1].state = NVS_CONST_PAGE_STATE_FREEING;
    item(1, 3, MOSAICO_BOOT_INTENT_KEY, NVS_TYPE_U32, MOSAICO_BOOT_INTENT_NONE);
    assert(mosaico_boot_read_intent(&intent) == ESP_OK && intent == MOSAICO_BOOT_INTENT_NONE);
    setup();
    pages[0].state = pages[1].state = NVS_CONST_PAGE_STATE_FREEING;
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_INVALID_STATE);
    setup();
    pages[2].error = ESP_ERR_NVS_INVALID_STATE; /* SDK rejected page CRC. */
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_NVS_NOT_FOUND);
    setup();
    partition_present = false;
    assert(mosaico_boot_read_intent(&intent) == ESP_ERR_NVS_PART_NOT_FOUND);
    assert(mosaico_boot_read_intent(NULL) == ESP_ERR_INVALID_ARG);
    return 0;
}
