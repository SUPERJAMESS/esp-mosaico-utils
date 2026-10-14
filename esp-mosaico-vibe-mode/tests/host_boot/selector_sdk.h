#pragma once
#include "sdk.h"
#include <stdlib.h>
#include <string.h>
#define CONFIG_FACTORY_RECOVERY_BOOT_GPIO 7
#define CONFIG_FACTORY_RECOVERY_BOOT_DEBOUNCE_MS 50
#define SOC_GPIO_VALID_GPIO_MASK (1ULL << 7)
#define RESET_REASON_CORE_DEEP_SLEEP 5
#define INVALID_INDEX (-99)
#define TEST_APP_INDEX (-2)
#define NVS_TYPE_U32 4
#define ESP_LOG_ATTR_TAG(name, text) static const char *name __attribute__((unused)) = text
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct { esp_partition_pos_t test, ota_info; } bootloader_state_t;
typedef struct {
    const char *namespace_name, *key_name;
    int value_type;
    esp_err_t result_code;
    union { uint32_t u32_val; } value;
} nvs_bootloader_read_list_t;
static int GPIO;
static int gpio_level = 1, selected, table_ok = 1;
static esp_err_t nvs_error, key_error = ESP_ERR_NVS_NOT_FOUND;
static uint32_t intent;
static uint8_t bootstrap[16];
static unsigned writes;
static bool usb_clock;
static unsigned usb_resets;
static inline bool _usb_utmi_ll_bus_clock_is_enabled(void) { return usb_clock; }
static inline void usb_utmi_ll_reset_register(void) { ++usb_resets; }
static inline void usb_utmi_ll_enable_bus_clock(bool enabled) { usb_clock = enabled; }
static inline esp_err_t bootloader_init(void) { return ESP_OK; }
static inline __attribute__((noreturn)) void bootloader_reset(void) { abort(); }
static inline esp_err_t mosaico_boot_splash_show(void) { return ESP_OK; }
static inline bool bootloader_utility_load_partition_table(bootloader_state_t *bs) { return table_ok; }
static inline __attribute__((noreturn)) void bootloader_utility_load_boot_image(bootloader_state_t *bs, int i) { abort(); }
static inline int bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs) { return selected; }
static inline esp_err_t bootloader_flash_read(size_t off, void *data, size_t size, bool decrypt)
{ memcpy(data, bootstrap, size); return ESP_OK; }
static inline esp_err_t nvs_bootloader_read(const char *p, size_t n, nvs_bootloader_read_list_t *e)
{ e->result_code = key_error; e->value.u32_val = intent; return nvs_error; }
static inline esp_err_t mosaico_boot_read_intent(uint32_t *value)
{ *value = intent; return nvs_error == ESP_OK ? key_error : nvs_error; }
static inline void esp_rom_gpio_pad_select_gpio(unsigned pin) { }
static inline void gpio_ll_input_enable(int *gpio, unsigned pin) { }
static inline void esp_rom_gpio_pad_pullup_only(unsigned pin) { }
static inline int gpio_ll_get_level(int *gpio, unsigned pin) { return gpio_level; }
static inline unsigned esp_log_early_timestamp(void) { static unsigned now; return ++now; }
static inline int esp_rom_get_reset_reason(int cpu) { return 0; }
