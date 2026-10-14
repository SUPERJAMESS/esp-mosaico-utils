#include "selector_sdk.h"
#include <assert.h>
#include SELECTOR_SOURCE

int main(void)
{
    stop_inherited_usb_dma();
    assert(!usb_resets); /* Cold boot: do not touch an unclocked controller. */
    usb_clock = true;
    stop_inherited_usb_dma();
    assert(usb_resets == 1 && !usb_clock);
    stop_inherited_usb_dma();
    assert(usb_resets == 1);
    bootloader_state_t bs = {
        .test = {MOSAICO_RECOVERY_ADDRESS, MOSAICO_RECOVERY_BYTES},
        .ota_info = {0x9000, MOSAICO_OTADATA_BYTES},
    };
    memset(bootstrap, 0xff, sizeof(bootstrap));
    assert(select_partition_number(&bs) == 0); /* Blank NVS: native app flash. */
    key_error = ESP_OK;
    intent = MOSAICO_BOOT_INTENT_RECOVERY;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    assert(select_partition_number(&bs) == TEST_APP_INDEX); /* Bootloader never consumes. */
    intent = MOSAICO_BOOT_INTENT_NONE;
    assert(select_partition_number(&bs) == 0);
    intent = 0xdeadbeef;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    intent = 0;
    key_error = ESP_FAIL;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    key_error = ESP_ERR_NVS_NOT_FOUND;
    nvs_error = ESP_FAIL;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    nvs_error = ESP_OK;
    memcpy(bootstrap, MOSAICO_BOOTSTRAP_MARKER, sizeof(bootstrap));
    assert(select_partition_number(&bs) == TEST_APP_INDEX); /* recover, valid old app present. */
    memset(bootstrap, 0xff, sizeof(bootstrap));
    gpio_level = 0;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    gpio_level = 1;
    selected = INVALID_INDEX;
    assert(select_partition_number(&bs) == TEST_APP_INDEX);
    bs.test.offset = 0; /* A wrong layout never grants a recovery boot target. */
    assert(select_partition_number(&bs) == INVALID_INDEX);
    table_ok = 0;
    assert(select_partition_number(&bs) == INVALID_INDEX);
    assert(writes == 0);
    return 0;
}
