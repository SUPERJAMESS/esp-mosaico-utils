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

/* Callback publication and protocol-worker snapshots share this lock. Never
 * call TinyUSB I/O under it: the stack has its own FIFO locks. Runtime pointers
 * are unpublished under the same lock before their owner can destroy them. */
static portMUX_TYPE s_link_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_configured, s_suspended;
static bool s_bus_event_seen;
static struct {
    uint32_t generation, cleaned_generation;
} s_link[IRIS_USB_CDC_COUNT];

static void usb_invalidate_locked(unsigned itf)
{
    ++s_link[itf].generation;
}

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
    taskENTER_CRITICAL(&s_link_lock);
    if (itf >= 0 && itf < IRIS_USB_CDC_COUNT && s_runtime[itf] != NULL)
        iris_notify_worker(s_runtime[itf]);
    taskEXIT_CRITICAL(&s_link_lock);
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    if (itf < 0 || itf >= IRIS_USB_CDC_COUNT || event == NULL) return;
    if (atomic_load(&s_shutting_down)) return;
    const bool opened = event->line_state_changed_data.dtr;
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    if (itf == 0) usb_download_line_state(opened, event->line_state_changed_data.rts);
#endif
    taskENTER_CRITICAL(&s_link_lock);
    if (s_state[itf]) {
        if (!opened) usb_invalidate_locked(itf);
        atomic_store(&s_state[itf]->host_open, opened);
        iris_notify_worker(s_runtime[itf]);
    }
    taskEXIT_CRITICAL(&s_link_lock);
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event == NULL || atomic_load(&s_shutting_down)) return;
    const bool attached = event->id == TINYUSB_EVENT_ATTACHED;
    const bool detached = event->id == TINYUSB_EVENT_DETACHED;
    const bool suspended = event->id == TINYUSB_EVENT_SUSPENDED;
    const bool resumed = event->id == TINYUSB_EVENT_RESUMED;
    if (!attached && !detached && !suspended && !resumed) return;
    unsigned line_state[IRIS_USB_CDC_COUNT] = {0};
    if (resumed) {
        for (unsigned i = 0; i < IRIS_USB_CDC_COUNT; ++i)
            line_state[i] = tud_cdc_n_get_line_state(i);
    }
#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    if (detached) s_download_sequence = (iris_usb_download_sequence_t){0};
#endif
    taskENTER_CRITICAL(&s_link_lock);
    s_bus_event_seen = true;
    /* Without VBUS sensing, unplug can look like suspend instead of detach.
     * Treat normal host suspend as the same protocol-session boundary too:
     * resume restores transport availability, then the host must re-handshake.
     * The first configuration has no preceding bus session to discard.
     * A repeated ATTACHED while configured is a reset boundary even when the
     * board cannot detect physical DETACHED (no VBUS input). */
    const bool invalidate = (attached && s_configured) || detached ||
        (suspended && !s_suspended);
    if (attached || detached) { s_configured = attached; s_suspended = false; }
    else s_suspended = suspended;
    for (unsigned i = 0; i < IRIS_USB_CDC_COUNT; ++i) {
        if (s_state[i] == NULL) continue;
        if (invalidate) usb_invalidate_locked(i);
        if (attached || detached) atomic_store(&s_state[i]->host_open, false);
        else if (resumed) atomic_store(&s_state[i]->host_open, (line_state[i] & 1U) != 0);
        iris_notify_worker(s_runtime[i]);
    }
    taskEXIT_CRITICAL(&s_link_lock);
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
        taskENTER_CRITICAL(&s_link_lock);
        s_configured = s_suspended = false;
        s_bus_event_seen = false;
        for (unsigned i = 0; i < IRIS_USB_CDC_COUNT; ++i) {
            s_link[i].generation = s_link[i].cleaned_generation = 0;
        }
        taskEXIT_CRITICAL(&s_link_lock);
        atomic_store(&s_shutting_down, false);
        esp_err_t err = tinyusb_driver_install(&config);
        if (err != ESP_OK) return err;
        /* Driver task can enumerate before esp_tinyusb installs its event
         * callback. Seed only if no event was published, never overwrite a
         * newer SUSPEND/DETACHED with this fallback snapshot. */
        const bool mounted = tud_mounted();
        const bool suspended = tud_suspended();
        taskENTER_CRITICAL(&s_link_lock);
        if (!s_bus_event_seen) {
            s_configured = mounted;
            s_suspended = suspended;
        }
        taskEXIT_CRITICAL(&s_link_lock);
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
    taskENTER_CRITICAL(&s_link_lock);
    s_runtime[itf] = runtime;
    s_state[itf] = state;
    /* A new stack/interface has no old session bytes. Do not flush its first
     * console command: native terminals have no handshake retry. Reopening an
     * interface while its peer keeps the stack alive still needs isolation. */
    const bool fresh = s_link[itf].generation == 0;
    ++s_link[itf].generation;
    if (fresh) s_link[itf].cleaned_generation = s_link[itf].generation;
    taskEXIT_CRITICAL(&s_link_lock);
    const tinyusb_config_cdcacm_t cdc = {
        .cdc_port = itf, .callback_rx = cdc_rx_callback,
        .callback_line_state_changed = cdc_line_state_callback,
    };
    esp_err_t err = tinyusb_cdcacm_init(&cdc);
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_link_lock);
        s_runtime[itf] = NULL;
        s_state[itf] = NULL;
        taskEXIT_CRITICAL(&s_link_lock);
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
    taskENTER_CRITICAL(&s_link_lock);
    s_runtime[itf] = NULL;
    s_state[itf] = NULL;
    taskEXIT_CRITICAL(&s_link_lock);
    if (--s_users == 0) usb_driver_uninstall();
    state->driver_started = false;
    atomic_store(&state->host_open, false);
    atomic_store(&state->disconnect_pending, false);
    state->link_up = state->reported_link_up = false;
}

static iris_link_event_t usb_poll(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (atomic_load(&s_shutting_down)) return IRIS_LINK_EVENT_NONE;
    const unsigned itf = interface_for(runtime);
    taskENTER_CRITICAL(&s_link_lock);
    const uint32_t generation = s_link[itf].generation;
    const bool clean = generation != s_link[itf].cleaned_generation;
    taskEXIT_CRITICAL(&s_link_lock);
    if (clean) {
        /* Discard old software queues before advertising the next session.
         * Early new-host bytes may be discarded too; the handshake retries.
         * Already submitted USB transfers cannot be recalled by FIFO clear. */
        tud_cdc_n_read_flush(itf);
        (void)tud_cdc_n_write_clear(itf);
    }
    taskENTER_CRITICAL(&s_link_lock);
    if (clean) s_link[itf].cleaned_generation = generation;
    const bool ready = generation == s_link[itf].generation &&
        s_configured && !s_suspended && state->driver_started &&
        (!runtime->data_link || atomic_load(&state->host_open));
    if (clean && state->reported_link_up) {
        state->link_up = state->reported_link_up = false;
        taskEXIT_CRITICAL(&s_link_lock);
        return IRIS_LINK_EVENT_DISCONNECTED;
    }
    state->link_up = ready;
    iris_link_event_t result = IRIS_LINK_EVENT_NONE;
    if (state->link_up != state->reported_link_up) {
        state->reported_link_up = state->link_up;
        result = state->link_up ? IRIS_LINK_EVENT_CONNECTED : IRIS_LINK_EVENT_DISCONNECTED;
    }
    taskEXIT_CRITICAL(&s_link_lock);
    return result;
}

static void usb_disconnect(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    (void)state;
    /* Logical rejection affects only this interface, never its healthy peer. */
    taskENTER_CRITICAL(&s_link_lock);
    usb_invalidate_locked(interface_for(runtime));
    taskEXIT_CRITICAL(&s_link_lock);
}

static bool usb_io_snapshot(iris_runtime_t *runtime, iris_transport_state_t *state,
                            uint32_t *generation)
{
    const unsigned itf = interface_for(runtime);
    taskENTER_CRITICAL(&s_link_lock);
    *generation = s_link[itf].generation;
    const bool ready = state->link_up && state->driver_started &&
        *generation == s_link[itf].cleaned_generation &&
        s_configured && !s_suspended && !atomic_load(&s_shutting_down) &&
        (!runtime->data_link || atomic_load(&state->host_open));
    taskEXIT_CRITICAL(&s_link_lock);
    return ready;
}

static int usb_read(iris_runtime_t *runtime, iris_transport_state_t *state,
                    uint8_t *buffer, size_t capacity)
{
    uint32_t generation, after;
    if (!usb_io_snapshot(runtime, state, &generation)) return -ENOTCONN;
    size_t received = 0;
    esp_err_t err = tinyusb_cdcacm_read(interface_for(runtime), buffer, capacity, &received);
    if (!usb_io_snapshot(runtime, state, &after) || after != generation) return -ENOTCONN;
    return err == ESP_OK ? (int)received : -EIO;
}

static int usb_write(iris_runtime_t *runtime, iris_transport_state_t *state,
                     const uint8_t *buffer, size_t length)
{
    uint32_t generation, after;
    if (!usb_io_snapshot(runtime, state, &generation)) return -ENOTCONN;
    const unsigned itf = interface_for(runtime);
    const size_t queued = tinyusb_cdcacm_write_queue(itf, buffer, length);
    if (queued > 0) (void)tinyusb_cdcacm_write_flush(itf, 0);
    if (!usb_io_snapshot(runtime, state, &after) || after != generation) return -ENOTCONN;
    return (int)queued;
}

const iris_transport_ops_t g_iris_usb_transport_ops = {
    .kind = ESP_IRIS_TRANSPORT_KIND_USB, .name = "usb",
    .start = usb_start, .stop = usb_stop, .poll = usb_poll,
    .disconnect = usb_disconnect, .read = usb_read, .write = usb_write,
};
