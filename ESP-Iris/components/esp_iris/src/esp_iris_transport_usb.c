#include "esp_iris_internal.h"

#include <errno.h>
#include "esp_log.h"
#include "esp_system.h"
#include "hal/usb_utmi_ll.h"
#include "device/dcd.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
#include "esp_timer.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "esp_iris_usb_download.h"

/* Adapted from esp-mosaico-bsp 3d0dcb5 usb_console.c. Keep the Iris PID and
 * one USB owner; the host extension explicitly sends the BSP reset sequence. */
static esp_timer_handle_t s_download_timer;
static atomic_bool s_download_requested;
static iris_usb_download_sequence_t s_download_sequence;

static void usb_download_restart(void *arg)
{
    (void)arg;
    if (atomic_load(&s_download_requested)) esp_restart();
}

static void usb_download_cancel(void)
{
    atomic_store(&s_download_requested, false);
    if (s_download_timer) (void)esp_timer_stop(s_download_timer);
    s_download_sequence = (iris_usb_download_sequence_t){0};
}

static void usb_download_line_state(bool dtr, bool rts)
{
    if (atomic_load(&s_download_requested)) {
        /* Let the final control transfer complete before USB disappears. */
        if (!dtr && !rts) (void)esp_timer_restart(s_download_timer, 50000);
        return;
    }
    if (iris_usb_download_sequence(&s_download_sequence, dtr, rts, esp_timer_get_time())) {
        atomic_store(&s_download_requested, true);
        /* Also finish when the host disconnects before releasing both lines. */
        if (esp_timer_start_once(s_download_timer, 500000) != ESP_OK)
            atomic_store(&s_download_requested, false);
    }
}
#endif

#if CONFIG_ESP_IRIS_DATA_LINK
#define IRIS_USB_CDC_COUNT 2
#else
#define IRIS_USB_CDC_COUNT 1
#endif
_Static_assert(CFG_TUD_CDC >= IRIS_USB_CDC_COUNT,
               "ESP-Iris 0.2 USB data requires CONFIG_TINYUSB_CDC_COUNT=2");

/* One USB stack, separate endpoints, decoders and link lifetimes. Only the
 * protocol task starts/stops interfaces; callbacks publish atomic events. */
static iris_runtime_t *s_runtime[IRIS_USB_CDC_COUNT];
static iris_transport_state_t *s_state[IRIS_USB_CDC_COUNT];
static unsigned s_users;
static atomic_bool s_shutting_down;
#define IRIS_USB_RHPORT 0U /* ESP32-S31's single DWC controller. */

/* S31's pinned SDK resets CPUs without resetting the HS USB DMA engine.
 * Its old RX/SETUP addresses may be code in the next application. Shutdown
 * can run on the Iris worker itself, so do not wait for that worker or free
 * TinyUSB queues here. Quiesce hardware without waiting for the protocol worker;
 * the imminent CPU reset ends the remaining task/resource lifetimes. */
static void usb_shutdown(void)
{
    if (atomic_exchange(&s_shutting_down, true)) return;
    dcd_int_disable(IRIS_USB_RHPORT);
    usb_utmi_ll_reset_register();
    usb_utmi_ll_enable_bus_clock(false);
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    /* Set the boot condition only after the old DMA engine is quiescent. */
    if (atomic_load(&s_download_requested))
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
#endif
}

static void usb_driver_uninstall(void)
{
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    usb_download_cancel();
    if (s_download_timer) {
        (void)esp_timer_delete(s_download_timer);
        s_download_timer = NULL;
    }
#endif
    const esp_err_t err = esp_unregister_shutdown_handler(usb_shutdown);
    if (err != ESP_OK) ESP_LOGW("iris_usb", "shutdown unregister: %s", esp_err_to_name(err));
    (void)tinyusb_driver_uninstall();
}
/* esp_tinyusb limits string descriptors to 31 characters. The complete
 * 12-digit eFuse MAC fits without truncating identity; HELLO carries device_id. */
static char s_serial[13];
static const char s_langid[] = {0x09, 0x04};
static const char *s_strings[] = {
    s_langid, CONFIG_ESP_IRIS_USB_MANUFACTURER, CONFIG_ESP_IRIS_USB_PRODUCT,
    s_serial, "ESP-Iris 0.2 console", "ESP-Iris 0.2 data",
};

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_ESP_IRIS_USB_VID, .idProduct = CONFIG_ESP_IRIS_USB_PID,
    .bcdDevice = 0x0200, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
#define IRIS_USB_CONFIG_BYTES (TUD_CONFIG_DESC_LEN + IRIS_USB_CDC_COUNT * TUD_CDC_DESC_LEN)
#define IRIS_USB_CONFIG_HEAD TUD_CONFIG_DESCRIPTOR(1, IRIS_USB_CDC_COUNT * 2, 0, \
    IRIS_USB_CONFIG_BYTES, 0, 100)
static const uint8_t s_fs_config[] = {
    IRIS_USB_CONFIG_HEAD,
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
#if CONFIG_ESP_IRIS_DATA_LINK
    TUD_CDC_DESCRIPTOR(2, 5, 0x83, 8, 0x04, 0x84, 64),
#endif
};
#if TUD_OPT_HIGH_SPEED
static const uint8_t s_hs_config[] = {
    IRIS_USB_CONFIG_HEAD,
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 512),
#if CONFIG_ESP_IRIS_DATA_LINK
    TUD_CDC_DESCRIPTOR(2, 5, 0x83, 8, 0x04, 0x84, 512),
#endif
};
#endif

static unsigned interface_for(const iris_runtime_t *runtime)
{
    return runtime->data_link ? 1U : 0U;
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (itf >= 0 && itf < IRIS_USB_CDC_COUNT && s_runtime[itf] != NULL)
        iris_notify_worker(s_runtime[itf]);
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    if (itf < 0 || itf >= IRIS_USB_CDC_COUNT || s_state[itf] == NULL) return;
    if (atomic_load(&s_shutting_down)) return;
    const bool opened = event->line_state_changed_data.dtr;
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    if (itf == 0) usb_download_line_state(opened, event->line_state_changed_data.rts);
#endif
    if (!opened) atomic_store(&s_state[itf]->disconnect_pending, true);
    atomic_store(&s_state[itf]->host_open, opened);
    cdc_rx_callback(itf, event);
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event == NULL || event->id != TINYUSB_EVENT_DETACHED) return;
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    s_download_sequence = (iris_usb_download_sequence_t){0};
#endif
    for (unsigned i = 0; i < IRIS_USB_CDC_COUNT; ++i) {
        if (s_state[i] == NULL) continue;
        atomic_store(&s_state[i]->host_open, false);
        atomic_store(&s_state[i]->disconnect_pending, true);
        if (s_runtime[i] != NULL) iris_notify_worker(s_runtime[i]);
    }
}

static esp_err_t usb_start(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL) return ESP_ERR_INVALID_ARG;
    if (state->driver_started) return ESP_OK;
    const unsigned itf = interface_for(runtime);
    if (itf >= IRIS_USB_CDC_COUNT) return ESP_ERR_NOT_SUPPORTED;
    if (s_state[itf] != NULL) return ESP_ERR_INVALID_STATE;
    if (s_users == 0) {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(runtime->hardware_mac); ++i) {
            s_serial[i * 2] = hex[runtime->hardware_mac[i] >> 4];
            s_serial[i * 2 + 1] = hex[runtime->hardware_mac[i] & 15];
        }
        s_serial[sizeof(s_serial) - 1] = '\0';
        tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_device_event_callback, NULL);
        config.task.size = CONFIG_ESP_IRIS_USB_TASK_STACK_SIZE;
        config.descriptor.device = &s_device;
        config.descriptor.string = s_strings;
        config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
        config.descriptor.full_speed_config = s_fs_config;
#if TUD_OPT_HIGH_SPEED
        config.descriptor.high_speed_config = s_hs_config;
#endif
        esp_err_t err = tinyusb_driver_install(&config);
        if (err != ESP_OK) return err;
        atomic_store(&s_shutting_down, false);
        err = esp_register_shutdown_handler(usb_shutdown);
        if (err != ESP_OK) {
            (void)tinyusb_driver_uninstall();
            return err;
        }
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
        const esp_timer_create_args_t timer = {
            .callback = usb_download_restart, .name = "iris_usb_rom",
        };
        err = esp_timer_create(&timer, &s_download_timer);
        if (err != ESP_OK) {
            usb_driver_uninstall();
            return err;
        }
#endif
    }
    atomic_store(&state->host_open, false);
    atomic_store(&state->disconnect_pending, false);
    s_runtime[itf] = runtime;
    s_state[itf] = state;
    const tinyusb_config_cdcacm_t cdc = {
        .cdc_port = itf, .callback_rx = cdc_rx_callback,
        .callback_line_state_changed = cdc_line_state_callback,
    };
    esp_err_t err = tinyusb_cdcacm_init(&cdc);
    if (err != ESP_OK) {
        s_runtime[itf] = NULL;
        s_state[itf] = NULL;
        if (s_users == 0) usb_driver_uninstall();
        return err;
    }
    ++s_users;
    state->driver_started = true;
    state->link_up = state->reported_link_up = false;
    return ESP_OK;
}

static void usb_stop(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL || !state->driver_started) return;
    if (atomic_load(&s_shutting_down)) return;
    const unsigned itf = interface_for(runtime);
    (void)tinyusb_cdcacm_deinit(itf);
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    if (itf == 0) usb_download_cancel();
#endif
    s_runtime[itf] = NULL;
    s_state[itf] = NULL;
    if (--s_users == 0) usb_driver_uninstall();
    state->driver_started = false;
    atomic_store(&state->host_open, false);
    atomic_store(&state->disconnect_pending, false);
    state->link_up = state->reported_link_up = false;
}

static iris_link_event_t usb_poll(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (atomic_load(&s_shutting_down)) return IRIS_LINK_EVENT_NONE;
    if (atomic_exchange(&state->disconnect_pending, false) && state->reported_link_up) {
        state->link_up = state->reported_link_up = false;
        return IRIS_LINK_EVENT_DISCONNECTED;
    }
    /* Stock IDF Monitor may leave DTR deasserted on application CDC. Text
     * console I/O must work whenever configured, without an Iris handshake.
     * Data retains DTR-based lifetime; deassertion still releases either role. */
    state->link_up = state->driver_started && tud_mounted() &&
        (!runtime->data_link || atomic_load(&state->host_open));
    if (state->link_up == state->reported_link_up) return IRIS_LINK_EVENT_NONE;
    state->reported_link_up = state->link_up;
    return state->link_up ? IRIS_LINK_EVENT_CONNECTED : IRIS_LINK_EVENT_DISCONNECTED;
}

static void usb_disconnect(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    (void)runtime;
    /* Logical rejection only. Never re-enumerate the healthy peer interface. */
    atomic_store(&state->disconnect_pending, true);
}

static int usb_read(iris_runtime_t *runtime, iris_transport_state_t *state,
                    uint8_t *buffer, size_t capacity)
{
    if (!state->link_up || atomic_load(&s_shutting_down)) return -ENOTCONN;
    size_t received = 0;
    esp_err_t err = tinyusb_cdcacm_read(interface_for(runtime), buffer, capacity, &received);
    return err == ESP_OK ? (int)received : -EIO;
}

static int usb_write(iris_runtime_t *runtime, iris_transport_state_t *state,
                     const uint8_t *buffer, size_t length)
{
    if (!state->link_up || atomic_load(&s_shutting_down)) return -ENOTCONN;
    const unsigned itf = interface_for(runtime);
    const size_t queued = tinyusb_cdcacm_write_queue(itf, buffer, length);
    if (queued > 0) (void)tinyusb_cdcacm_write_flush(itf, 0);
    return (int)queued;
}

const iris_transport_ops_t g_iris_usb_transport_ops = {
    .kind = ESP_IRIS_TRANSPORT_KIND_USB, .name = "usb",
    .start = usb_start, .stop = usb_stop, .poll = usb_poll,
    .disconnect = usb_disconnect, .read = usb_read, .write = usb_write,
};
