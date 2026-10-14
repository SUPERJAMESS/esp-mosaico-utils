/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <inttypes.h>
#include <sys/reent.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_caps.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "hal/gpio_ll.h"
#include "hal/usb_utmi_ll.h"
#include "mosaico_boot_splash.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"
#include "mosaico_boot_nvs.h"
#include "nvs.h"
#include "mosaico_recovery_contract.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

static void stop_inherited_usb_dma(void)
{
    /* Panic/watchdog resets bypass application shutdown handlers. S31's SDK
     * leaves USB DMA running across CPU resets: stop it before loading any
     * application RAM segments that can overlap the previous USB buffers. */
    if (_usb_utmi_ll_bus_clock_is_enabled()) {
        usb_utmi_ll_reset_register();
        usb_utmi_ll_enable_bus_clock(false);
    }
}

static bool bootstrap_recovery_requested(const bootloader_state_t *bs)
{
    if (bs->ota_info.size != MOSAICO_OTADATA_BYTES) return false;
    char marker[MOSAICO_BOOTSTRAP_BYTES];
    return bootloader_flash_read(bs->ota_info.offset + MOSAICO_BOOTSTRAP_OFFSET,
        marker, sizeof(marker), true) == ESP_OK &&
        memcmp(marker, MOSAICO_BOOTSTRAP_MARKER, sizeof(marker)) == 0;
}

static bool software_recovery_requested(void)
{
    uint32_t intent = MOSAICO_BOOT_INTENT_NONE;
    esp_err_t err = mosaico_boot_read_intent(&intent);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK || (intent != MOSAICO_BOOT_INTENT_NONE &&
                          intent != MOSAICO_BOOT_INTENT_RECOVERY)) {
        ESP_LOGW(TAG, "Boot intent unavailable (%d); selecting Vibe Mode", err);
        return true;
    }
    return intent == MOSAICO_BOOT_INTENT_RECOVERY;
}

/* The AI button selects Vibe Mode after ROM boots this loader from Flash.
 * The separate Boot button selects ROM Download Mode before this code runs. */
static bool factory_recovery_requested(void)
{
    const uint32_t pin = CONFIG_FACTORY_RECOVERY_BOOT_GPIO;

    if (((1ULL << pin) & SOC_GPIO_VALID_GPIO_MASK) == 0) {
        ESP_LOGE(TAG, "Factory recovery GPIO %" PRIu32 " is not a valid input", pin);
        return false;
    }

    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_input_enable(&GPIO, pin);
    esp_rom_gpio_pad_pullup_only(pin);

    if (gpio_ll_get_level(&GPIO, pin) != 0) {
        return false;
    }

    const uint32_t started_ms = esp_log_early_timestamp();
    while ((esp_log_early_timestamp() - started_ms) <
           CONFIG_FACTORY_RECOVERY_BOOT_DEBOUNCE_MS) {
        if (gpio_ll_get_level(&GPIO, pin) != 0) {
            ESP_LOGI(TAG, "Ignoring short low pulse on factory recovery GPIO %" PRIu32, pin);
            return false;
        }
    }

    return true;
}

/*
 * We arrive here after the ROM bootloader has loaded this second-stage
 * bootloader from flash. This remains aligned with ESP-IDF's default
 * bootloader entry flow; only partition selection is extended below.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    stop_inherited_usb_dma();
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    /* The retained Recovery bootloader owns the product's first visible
     * frame. Splash failures are deliberately non-fatal so display hardware
     * can never prevent Recovery or the normal application from booting. */
    (void)mosaico_boot_splash_show();

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    bootloader_utility_load_boot_image(&bs, boot_index);
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    if (factory_recovery_requested() || bootstrap_recovery_requested(bs) ||
        software_recovery_requested()) {
        if (bs->test.offset == MOSAICO_RECOVERY_ADDRESS &&
            bs->test.size == MOSAICO_RECOVERY_BYTES) {
            ESP_LOGW(TAG,
                     "Maintenance requested; booting Vibe Mode (GPIO%d), preserving OTA data",
                     CONFIG_FACTORY_RECOVERY_BOOT_GPIO);
            return TEST_APP_INDEX;
        }
        ESP_LOGE(TAG, "Vibe Mode test partition is missing or has the wrong layout");
    }

    const int boot_index = selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX && bs->test.offset == MOSAICO_RECOVERY_ADDRESS &&
        bs->test.size == MOSAICO_RECOVERY_BYTES) return TEST_APP_INDEX;
    ESP_LOGI(TAG, "Selected boot partition index=%d", boot_index);
    return boot_index;
}

/* Keep ESP-IDF's standard OTA, rollback, factory-reset, and test-app rules. */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index;
    }

    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
        bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
        reset_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(
                CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET,
                CONFIG_BOOTLOADER_HOLD_TIME_GPIO,
                reset_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a condition of the factory reset");
            bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
            ota_data_erase = true;
#endif
            const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
            ESP_LOGI(TAG, "Data partitions to erase: %s", list_erase);
            if (!bootloader_common_erase_part_type_data(list_erase, ota_data_erase)) {
                ESP_LOGE(TAG, "Not all partitions were erased");
            }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
            bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
            return bootloader_utility_get_selected_boot_partition(bs);
        }
#endif

#ifdef CONFIG_BOOTLOADER_APP_TEST
        bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
        app_test_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(
                CONFIG_BOOTLOADER_NUM_PIN_APP_TEST,
                CONFIG_BOOTLOADER_HOLD_TIME_GPIO,
                app_test_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a boot condition of the test firmware");
            if (bs->test.offset != 0) {
                return TEST_APP_INDEX;
            }
            ESP_LOGE(TAG, "Test firmware is not found in partition table");
            return INVALID_INDEX;
        }
#endif
    }

    return boot_index;
}

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
