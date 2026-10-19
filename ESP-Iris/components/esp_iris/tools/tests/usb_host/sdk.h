#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define CONFIG_ESP_IRIS_DATA_LINK 1
#define CFG_TUD_CDC 2
#define TUD_OPT_HIGH_SPEED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CONFIG_ESP_IRIS_USB_MANUFACTURER "test"
#define CONFIG_ESP_IRIS_USB_PRODUCT "test"
#define CONFIG_ESP_IRIS_USB_VID 1
#define CONFIG_ESP_IRIS_USB_PID 2
#define CONFIG_ESP_IRIS_USB_TASK_STACK_SIZE 4096
#define TUSB_DESC_DEVICE 1
#define TUSB_CLASS_MISC 2
#define MISC_SUBCLASS_COMMON 3
#define MISC_PROTOCOL_IAD 4
#define TUD_CONFIG_DESC_LEN 1
#define TUD_CDC_DESC_LEN 1
#define TUD_CONFIG_DESCRIPTOR(...) 0
#define TUD_CDC_DESCRIPTOR(...) 0
#define TINYUSB_EVENT_DETACHED 1
#define TINYUSB_EVENT_ATTACHED 0
#define TINYUSB_EVENT_SUSPENDED 2
#define TINYUSB_EVENT_RESUMED 3
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { bool data_link; uint8_t hardware_mac[6]; } iris_runtime_t;
typedef struct {
    bool driver_started, link_up, reported_link_up;
    atomic_bool host_open, disconnect_pending;
} iris_transport_state_t;
typedef enum { IRIS_LINK_EVENT_NONE, IRIS_LINK_EVENT_CONNECTED,
               IRIS_LINK_EVENT_DISCONNECTED } iris_link_event_t;
#define ESP_IRIS_TRANSPORT_KIND_USB 1
typedef struct {
    int kind;
    const char *name;
    esp_err_t (*start)(iris_runtime_t *, iris_transport_state_t *);
    void (*stop)(iris_runtime_t *, iris_transport_state_t *);
    iris_link_event_t (*poll)(iris_runtime_t *, iris_transport_state_t *);
    void (*disconnect)(iris_runtime_t *, iris_transport_state_t *);
    int (*read)(iris_runtime_t *, iris_transport_state_t *, uint8_t *, size_t);
    int (*write)(iris_runtime_t *, iris_transport_state_t *, const uint8_t *, size_t);
} iris_transport_ops_t;
typedef struct {
    unsigned bLength, bDescriptorType, bcdUSB, bDeviceClass, bDeviceSubClass,
        bDeviceProtocol, bMaxPacketSize0, idVendor, idProduct, bcdDevice,
        iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;
typedef struct { struct { bool dtr, rts; } line_state_changed_data; } cdcacm_event_t;
typedef struct { int id; } tinyusb_event_t;
typedef struct {
    struct { unsigned size; } task;
    struct {
        const void *device;
        const char **string;
        size_t string_count;
        const uint8_t *full_speed_config, *high_speed_config;
    } descriptor;
    void (*callback)(tinyusb_event_t *, void *);
} tinyusb_config_t;
#define TINYUSB_DEFAULT_CONFIG(cb, arg) { .callback = cb }
typedef struct {
    unsigned cdc_port;
    void (*callback_rx)(int, cdcacm_event_t *);
    void (*callback_line_state_changed)(int, cdcacm_event_t *);
} tinyusb_config_cdcacm_t;

static void (*shutdown_handler)(void);
static unsigned installs, uninstalls, registrations, removals, io_calls, stop_step;
static bool fail_install, fail_register, fail_cdc, clock_enabled;
static void iris_notify_worker(iris_runtime_t *runtime) { }
static bool fake_suspended;
static bool fake_mounted = true;
static int install_events[3], install_event_count;
static esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    ++installs; clock_enabled = !fail_install;
    if (fail_install) return ESP_FAIL;
    for (int i = 0; i < install_event_count; ++i) {
        tinyusb_event_t event = {.id = install_events[i]};
        config->callback(&event, NULL);
    }
    return ESP_OK;
}
static esp_err_t tinyusb_driver_uninstall(void)
{ ++uninstalls; clock_enabled = false; return ESP_OK; }
static esp_err_t esp_register_shutdown_handler(void (*handler)(void))
{
    ++registrations;
    if (fail_register) return ESP_FAIL;
    assert(shutdown_handler == NULL);
    shutdown_handler = handler;
    return ESP_OK;
}
static esp_err_t esp_unregister_shutdown_handler(void (*handler)(void))
{ assert(shutdown_handler == handler); ++removals; shutdown_handler = NULL; return ESP_OK; }
static esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config)
{ return fail_cdc ? ESP_FAIL : ESP_OK; }
static esp_err_t tinyusb_cdcacm_deinit(unsigned itf) { return ESP_OK; }
static bool tud_mounted(void) { return fake_mounted; }
static bool tud_suspended(void) { return fake_suspended; }
static unsigned line_states[2], rx_clear[2], tx_clear[2];
static unsigned rx_bytes[2], tx_bytes[2];
static void (*io_hook)(void);
static void (*flush_hook)(void);
static unsigned tud_cdc_n_get_line_state(unsigned itf) { return line_states[itf]; }
static void tud_cdc_n_read_flush(unsigned itf) { ++rx_clear[itf]; rx_bytes[itf] = 0; if (flush_hook) flush_hook(); }
static bool tud_cdc_n_write_clear(unsigned itf) { ++tx_clear[itf]; tx_bytes[itf] = 0; return true; }
static esp_err_t tinyusb_cdcacm_read(unsigned itf, uint8_t *data, size_t size, size_t *received)
{ ++io_calls; *received = rx_bytes[itf] < size ? rx_bytes[itf] : size; rx_bytes[itf] -= *received; if (io_hook) io_hook(); return ESP_OK; }
static size_t tinyusb_cdcacm_write_queue(unsigned itf, const uint8_t *data, size_t size)
{ ++io_calls; tx_bytes[itf] += size; if (io_hook) io_hook(); return size; }
static esp_err_t tinyusb_cdcacm_write_flush(unsigned itf, unsigned timeout) { return ESP_OK; }
static void dcd_int_disable(unsigned rhport)
{ assert(rhport == 0 && stop_step == 0); stop_step = 1; }
static void usb_utmi_ll_reset_register(void)
{ assert(stop_step == 1 && clock_enabled); stop_step = 2; }
static void usb_utmi_ll_enable_bus_clock(bool enabled)
{ assert(stop_step == 2 && !enabled); stop_step = 3; clock_enabled = false; }

#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
#define LP_SYSTEM_REG_SYS_CTRL_REG 1
#define LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT 4
static unsigned download_boot, restart_calls, timers;
static int64_t now_us;
static bool fail_timer;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { void (*callback)(void *); uint64_t delay; bool active; } fake_timer_t;
typedef fake_timer_t *esp_timer_handle_t;
static fake_timer_t fake_timer;
static int64_t esp_timer_get_time(void) { return now_us; }
static esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    if (fail_timer) return ESP_FAIL;
    ++timers; fake_timer = (fake_timer_t){.callback = args->callback};
    *timer = &fake_timer; return ESP_OK;
}
static esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ timer->active = false; return ESP_OK; }
static esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{ assert(!timer->active); --timers; return ESP_OK; }
static esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay)
{ timer->delay = delay; timer->active = true; return ESP_OK; }
static esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t delay)
{ return esp_timer_start_once(timer, delay); }
static void set_download_boot(unsigned reg, unsigned value)
{ assert(reg == 1 && stop_step == 3 && !clock_enabled); download_boot |= value; }
#define REG_SET_BIT(reg, bit) set_download_boot(reg, bit)
static void esp_restart(void) { ++restart_calls; shutdown_handler(); }
#endif
