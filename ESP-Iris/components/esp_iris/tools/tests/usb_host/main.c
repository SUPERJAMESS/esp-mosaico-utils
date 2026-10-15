#include "sdk.h"
#include USB_SOURCE

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
    usb_stop(&control, &c);
    assert(s_users == 1 && shutdown_handler && uninstalls == 2);
    usb_stop(&data, &d);
    assert(!s_users && !shutdown_handler && uninstalls == 3 && removals == 2);

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
