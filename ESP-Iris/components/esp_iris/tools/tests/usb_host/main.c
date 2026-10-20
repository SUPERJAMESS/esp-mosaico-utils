#include "sdk.h"
#include USB_SOURCE

static void monitor_open(void)
{
    /* The stock monitor ends open_serial() with both lines deasserted. */
    cdcacm_event_t event = {.line_state_changed_data = {.dtr = true, .rts = true}};
    cdc_line_state_callback(0, &event);
    event.line_state_changed_data.dtr = false;
    event.line_state_changed_data.rts = false;
    cdc_line_state_callback(0, &event);
    delay_hook = NULL;
}

static void test_console_startup(iris_runtime_t *control, iris_runtime_t *data,
                                 iris_transport_state_t *c, iris_transport_state_t *d)
{
    assert(usb_start(control, c) == ESP_OK);
    assert(usb_start(data, d) == ESP_OK);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_NONE);
    assert(!usb_console_ready()); /* Enumeration is not a terminal open. */
    TickType_t start = ticks;
    iris_usb_console_wait();
    assert(ticks - start == pdMS_TO_TICKS(CONFIG_ESP_IRIS_USB_CONSOLE_HOST_WAIT_MS));
    /* Opening the independent data interface must not consume console logs. */
    cdcacm_event_t event = {.line_state_changed_data = {.dtr = true, .rts = true}};
    cdc_line_state_callback(1, &event);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_NONE);

    start = ticks;
    delay_hook = monitor_open;
#if CONFIG_ESP_IRIS_USB_CONSOLE_HOST_WAIT_MS > 0
    iris_usb_console_wait();
    assert(ticks - start < pdMS_TO_TICKS(CONFIG_ESP_IRIS_USB_CONSOLE_HOST_WAIT_MS));
    assert(usb_console_ready() && !atomic_load(&c->host_open));
#else
    iris_usb_console_wait();
    assert(ticks == start);
    monitor_open();
    assert(!usb_console_ready());
    ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
#endif
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    /* Detach removes the open latch; the same tty name alone is not enough. */
    fake_mounted = false;
    tinyusb_event_t detached = {.id = TINYUSB_EVENT_DETACHED};
    usb_device_event_callback(&detached, NULL);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    fake_mounted = true;
    tinyusb_event_t attached = {.id = TINYUSB_EVENT_ATTACHED};
    usb_device_event_callback(&attached, NULL);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_NONE);
    /* RX is also explicit evidence of a reader, even with no DTR assertion. */
    cdc_rx_callback(0, NULL);
    ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    /* Tick wrap must not turn a bounded startup wait into a hang. */
    usb_device_event_callback(&detached, NULL);
    ticks = UINT32_MAX - pdMS_TO_TICKS(20);
    start = ticks;
    iris_usb_console_wait();
    assert((TickType_t)(ticks - start) == pdMS_TO_TICKS(CONFIG_ESP_IRIS_USB_CONSOLE_HOST_WAIT_MS));
    usb_stop(control, c);
    usb_stop(data, d);
}

#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
static void lines(unsigned itf, bool dtr, bool rts, unsigned elapsed_ms)
{
    now_us += elapsed_ms * 1000;
    cdcacm_event_t event = {.line_state_changed_data = {.dtr = dtr, .rts = rts}};
    cdc_line_state_callback(itf, &event);
}

static void test_download(iris_runtime_t *control, iris_runtime_t *data,
                          iris_transport_state_t *c, iris_transport_state_t *d)
{
    /* A failed timer allocation must release the driver and shutdown hook. */
    fail_timer = true;
    assert(usb_start(control, c) == ESP_FAIL);
    assert(!timers && !shutdown_handler && !s_users);
    fail_timer = false;
    assert(usb_start(control, c) == ESP_OK);
    assert(usb_start(data, d) == ESP_OK);
    assert(timers == 1);
    /* Even a complete reset sequence on CDC1 must not reset the chip. */
    lines(1, false, false, 0);
    lines(1, true, false, 100);
    lines(1, true, true, 100);
    lines(1, false, true, 0);
    lines(1, false, false, 100);
    assert(!fake_timer.active && !restart_calls);
    /* Monitor/Gateway no-reset open, close, and normal RTS pulse. */
    lines(0, true, true, 0);
    lines(0, true, false, 0);
    lines(0, false, false, 0);
    lines(0, false, true, 100);
    lines(0, false, false, 5);
    assert(!fake_timer.active);
    /* Stale and too-short transitions cannot request ROM. */
    lines(0, true, false, 0);
    lines(0, false, true, 2000);
    lines(0, false, false, 0);
    lines(0, true, false, 0);
    lines(0, false, true, 1);
    lines(0, false, false, 0);
    assert(!fake_timer.active);
    /* Classic reset with its intermediate (1,1) control transfer. */
    lines(0, false, true, 0);
    lines(0, true, true, 100);
    lines(0, true, false, 0);
    assert(fake_timer.active && fake_timer.delay == 500000);
    lines(0, false, false, 50);
    assert(fake_timer.delay == 50000);
    usb_stop(control, c);
    assert(!fake_timer.active && timers == 1 && !atomic_load(&s_download_requested));
    assert(usb_start(control, c) == ESP_OK);
    /* Unix tight reset updates both lines in one transfer. */
    lines(0, false, true, 0);
    lines(0, true, false, 100);
    assert(fake_timer.active);
    usb_download_cancel();
    /* Old BSP USBJTAGSerialReset, including repeated Windows requests. */
    lines(0, false, false, 0);
    lines(0, true, false, 100);
    lines(0, true, false, 0);
    lines(0, true, true, 100);
    lines(0, false, true, 0);
    lines(0, false, true, 0);
    assert(fake_timer.delay == 500000);
    /* Detach must clear partial handshakes, but preserve a confirmed request. */
    tinyusb_event_t detached = {.id = TINYUSB_EVENT_DETACHED};
    usb_device_event_callback(&detached, NULL);
    assert(fake_timer.active);
    lines(0, false, false, 100);
    assert(fake_timer.delay == 50000 && !download_boot);
    fake_timer.callback(NULL);
    assert(restart_calls == 1 && download_boot == LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    assert(stop_step == 3 && !clock_enabled); /* DMA stopped before boot bit. */
}
#endif

static void bus_event(int id)
{
    tinyusb_event_t event = {.id = id};
    usb_device_event_callback(&event, NULL);
}

static void dtr(unsigned itf, bool open)
{
    line_states[itf] = open;
    cdcacm_event_t event = {.line_state_changed_data = {.dtr = open}};
    cdc_line_state_callback(itf, &event);
    ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
}

static void suspend_during_io(void)
{
    io_hook = NULL;
    bus_event(TINYUSB_EVENT_SUSPENDED);
}

static void resume_during_flush(void)
{
    flush_hook = NULL;
    bus_event(TINYUSB_EVENT_SUSPENDED);
    bus_event(TINYUSB_EVENT_RESUMED);
}

static void test_sessions(iris_runtime_t *control, iris_runtime_t *data,
                          iris_transport_state_t *c, iris_transport_state_t *d)
{
    bus_event(TINYUSB_EVENT_ATTACHED);
    dtr(0, true);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    dtr(1, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    uint8_t byte = 0;
    rx_bytes[0] = rx_bytes[1] = tx_bytes[0] = tx_bytes[1] = 7;
    bus_event(TINYUSB_EVENT_SUSPENDED);
    assert(usb_read(data, d, &byte, 1) == -ENOTCONN);
    assert(usb_write(control, c, &byte, 1) == -ENOTCONN);
    unsigned gen = s_link[1].generation;
    bus_event(TINYUSB_EVENT_SUSPENDED);
    assert(s_link[1].generation == gen);
    bus_event(TINYUSB_EVENT_RESUMED); /* Entire DOWN/UP between worker polls. */
    ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(!rx_bytes[0] && !rx_bytes[1] && !tx_bytes[0] && !tx_bytes[1]);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    /* Reset/reconfigure without physical DETACHED must release both sessions
     * and must not trust a cached DTR from the prior configuration. */
    bus_event(TINYUSB_EVENT_ATTACHED);
    dtr(0, true);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    dtr(1, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    dtr(1, false); dtr(1, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_NONE);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    dtr(0, false); /* Console close releases lifetime even without DTR=1. */
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    /* Read/write that race invalidation cannot be credited to a new session. */
    io_hook = suspend_during_io;
    assert(usb_read(data, d, &byte, 1) == -ENOTCONN);
    line_states[1] = 0; /* Resume must refresh DTR, not keep its cached true. */
    bus_event(TINYUSB_EVENT_RESUMED);
    dtr(0, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    dtr(1, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    io_hook = suspend_during_io;
    assert(usb_write(data, d, &byte, 1) == -ENOTCONN);
    bus_event(TINYUSB_EVENT_RESUMED);
    dtr(0, true);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(!tx_bytes[1]);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_CONNECTED);
    bus_event(TINYUSB_EVENT_DETACHED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
    bus_event(TINYUSB_EVENT_ATTACHED);
    dtr(0, true);
    (void)usb_poll(control, c);
    (void)usb_poll(control, c);
    assert(c->reported_link_up);
    usb_disconnect(control, c);
    flush_hook = resume_during_flush;
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(s_link[0].cleaned_generation != s_link[0].generation);
    assert(usb_write(control, c, &byte, 1) == -ENOTCONN);
    ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
    assert(usb_poll(control, c) == IRIS_LINK_EVENT_CONNECTED);
    assert(s_link[0].cleaned_generation == s_link[0].generation);
    io_calls = 0; /* Remaining shutdown regression expects no successful I/O. */
}

static void test_install_events(iris_runtime_t *control, iris_runtime_t *data,
                                iris_transport_state_t *c, iris_transport_state_t *d)
{
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        /* 0: callback omitted, already suspended; 1: ATTACHED before init;
         * 2: ATTACHED,SUSPEND contradict mounted fallback;
         * 3: complete suspend/resume before CDC publication;
         * 4: first ATTACHED after CDC publication, before first poll. */
        fake_suspended = scenario == 0;
        fake_mounted = scenario != 4;
        install_event_count = (scenario == 0 || scenario == 4) ? 0 : (int)scenario;
        install_events[0] = TINYUSB_EVENT_ATTACHED;
        install_events[1] = TINYUSB_EVENT_SUSPENDED;
        install_events[2] = TINYUSB_EVENT_RESUMED;
        assert(usb_start(control, c) == ESP_OK);
        assert(usb_start(data, d) == ESP_OK);
        if (scenario == 4) bus_event(TINYUSB_EVENT_ATTACHED); /* First late configuration. */
        rx_bytes[0] = 1; /* First native console command, sent before poll. */
        cdc_rx_callback(0, NULL);
        ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
        const unsigned clears = rx_clear[0];
        const bool suspended = scenario == 0 || scenario == 2;
        assert(usb_poll(control, c) == (suspended ? IRIS_LINK_EVENT_NONE : IRIS_LINK_EVENT_CONNECTED));
        assert(rx_bytes[0] == 1 && rx_clear[0] == clears);
        assert(usb_poll(data, d) == IRIS_LINK_EVENT_NONE);
        if (suspended) {
            bus_event(TINYUSB_EVENT_RESUMED);
            cdc_rx_callback(0, NULL);
            ticks += pdMS_TO_TICKS(IRIS_USB_CONSOLE_SETTLE_MS);
        }
        (void)usb_poll(control, c);
        uint8_t byte;
        assert(usb_read(control, c, &byte, 1) == 1);
        usb_stop(control, c);
        /* Restart one CDC while peer holds the stack: old queued bytes must
         * not be mistaken for a fresh interface's first command. */
        rx_bytes[0] = 2;
        assert(usb_start(control, c) == ESP_OK);
        (void)usb_poll(control, c);
        assert(rx_bytes[0] == 0);
        usb_stop(control, c);
        usb_stop(data, d);
        bus_event(TINYUSB_EVENT_DETACHED); /* No published runtime pointers. */
    }
    install_event_count = 0;
    fake_suspended = false;
    fake_mounted = true;
    io_calls = 0;
}

int main(void)
{
    iris_runtime_t control = {0}, data = {.data_link = true};
    iris_transport_state_t c = {0}, d = {0};
    fail_install = true;
    assert(usb_start(&control, &c) == ESP_FAIL);
    assert(!registrations && !shutdown_handler && !c.driver_started);
    fail_install = false;
    fail_register = true;
    assert(usb_start(&control, &c) == ESP_FAIL);
    assert(uninstalls == 1 && !s_users && !shutdown_handler);
    fail_register = false;
    fail_cdc = true;
    assert(usb_start(&control, &c) == ESP_FAIL);
    assert(uninstalls == 2 && removals == 1 && !shutdown_handler);
    fail_cdc = false;
    assert(usb_start(&control, &c) == ESP_OK);
    unsigned installed = installs, registered = registrations;
    fail_cdc = true;
    assert(usb_start(&data, &d) == ESP_FAIL);
    assert(installs == installed && registrations == registered && shutdown_handler);
    fail_cdc = false;
    assert(usb_start(&data, &d) == ESP_OK);
    assert(installs == installed && registrations == registered && s_users == 2);
    test_sessions(&control, &data, &c, &d);
    usb_stop(&control, &c);
    assert(s_users == 1 && shutdown_handler && uninstalls == 2);
    usb_stop(&data, &d);
    assert(!s_users && !shutdown_handler && uninstalls == 3 && removals == 2);
    test_install_events(&control, &data, &c, &d);

    test_console_startup(&control, &data, &c, &d);

#if CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD
    test_download(&control, &data, &c, &d);
#else
    assert(usb_start(&control, &c) == ESP_OK);
#endif
    c.link_up = true;
    shutdown_handler();
    assert(stop_step == 3 && !clock_enabled);
    uint8_t byte = 0;
    assert(usb_read(&control, &c, &byte, 1) == -ENOTCONN);
    assert(usb_write(&control, &c, &byte, 1) == -ENOTCONN);
    assert(usb_poll(&control, &c) == IRIS_LINK_EVENT_NONE && io_calls == 0);
    shutdown_handler(); /* Idempotent; do not touch the stopped controller. */
    usb_stop(&control, &c); /* No queue teardown against unclocked hardware. */
    assert(stop_step == 3);
    return 0;
}
