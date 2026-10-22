/* Hardware and unrelated providers only; runtime, services, codec and
 * transport arbitration below run their production implementations. */
unsigned uxTaskGetStackHighWaterMark2(void *p) { return 9999; }
static unsigned (*notify_take_hook)(unsigned ticks);
static void (*task_delay_hook)(unsigned ticks);
unsigned ulTaskNotifyTake(int b, unsigned n) {
    return notify_take_hook != NULL ? notify_take_hook(n) : 0;
}
void vTaskDelete(void *p) { }
static bool fail_task_create;
int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *p,unsigned pri,TaskHandle_t *t) {
    if (fail_task_create) return 0;
    *t = (void *)1; return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
void xTaskNotifyGive(TaskHandle_t t) { }
unsigned xTaskGetTickCount(void) { return 0; }
void vTaskDelay(unsigned n) { if (task_delay_hook != NULL) task_delay_hook(n); }
const esp_app_desc_t *esp_app_get_description(void) { static esp_app_desc_t d; return &d; }
size_t heap_caps_get_free_size(unsigned c) { return 10000; }
size_t heap_caps_get_minimum_free_size(unsigned c) { return 10000; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_UNKNOWN; }
static bool allow_restart;
static unsigned restart_calls;
void esp_restart(void) { assert(allow_restart); ++restart_calls; }
esp_err_t iris_identity_load_or_create(iris_runtime_t *r) { return ESP_OK; }
esp_err_t iris_log_vfs_init(iris_runtime_t *r) { return ESP_OK; }
esp_err_t iris_log_vfs_deinit(void) { return ESP_OK; }
esp_err_t iris_log_redirect_stdio(void) { return ESP_OK; }
esp_err_t iris_log_restore_stdio(void) { return ESP_OK; }
static unsigned log_capture_starts, usb_console_waits;
esp_err_t iris_log_capture_start(iris_runtime_t *r) {
    if (!r->stdio_redirected) ++log_capture_starts;
    r->stdio_redirected = r->vfs_registered = true;
    return ESP_OK;
}
esp_err_t iris_log_capture_stop(iris_runtime_t *r) {
    r->stdio_redirected = r->vfs_registered = false;
    return ESP_OK;
}
void iris_usb_console_wait(void) { ++usb_console_waits; }
static unsigned pending_test_logs;
bool iris_log_pop(iris_runtime_t *r, size_t n, iris_log_record_t *o) {
    if (!pending_test_logs) return false;
    --pending_test_logs;
    *o = (iris_log_record_t){.length = 5};
    memcpy(o->data, "boot\n", 5);
    return true;
}
void iris_crash_probe(iris_runtime_t *r) { }
void iris_crash_context_prepare(iris_runtime_t *r) { }
esp_err_t iris_crash_recovery_probe(iris_runtime_t *r) {
    r->crash_loop_initialized = true; r->crash_limit = 3; return ESP_OK;
}
esp_err_t iris_crash_recovery_mark_planned(iris_runtime_t *r) { return ESP_OK; }
esp_err_t iris_crash_recovery_mark_healthy(iris_runtime_t *r) {
    r->crash_count = 0; r->crash_loop_triggered = false;
    r->crash_recovery_pending = false; return ESP_OK;
}
esp_err_t iris_crash_recovery_reset(iris_runtime_t *r) {
    return iris_crash_recovery_mark_healthy(r);
}
void iris_crash_recovery_poll(iris_runtime_t *r, int64_t now) { }
esp_err_t iris_crash_build_metadata(iris_runtime_t *r, uint8_t *o, size_t n, size_t *s) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t iris_crash_read(iris_runtime_t *r, size_t a, size_t n, uint8_t *o, size_t *s) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t iris_files_init(iris_runtime_t *r) { return ESP_OK; }
void iris_files_deinit(void) { }
void iris_files_session_end(uint32_t s) { }
#if !CONFIG_ESP_IRIS_SYSTEM_UPDATE
bool iris_system_update_backend_registered(void) { return false; }
void iris_system_update_session_end(void) { }
esp_err_t iris_system_update_request_cancel(const uint8_t *p, size_t n) { return ESP_ERR_NOT_SUPPORTED; }
void iris_system_update_cancel_all(void) { }
bool iris_system_update_cancel_pending(void) { return false; }
void iris_system_update_poll_cancel(void) { }
#endif
bool iris_files_handle_frame(iris_runtime_t *r, const iris_decoded_frame_t *f) { return false; }
#if !CONFIG_ESP_IRIS_SYSTEM_UPDATE
bool iris_system_update_handle_frame(iris_service_call_t *r, const iris_decoded_frame_t *f) { return false; }
#endif
bool iris_files_queue_next(iris_runtime_t *r) { return false; }
uint64_t iris_files_capabilities(void) { return 0; }
#if !CONFIG_ESP_IRIS_SYSTEM_UPDATE
uint64_t iris_system_update_capabilities(void) { return 0; }
#endif
uint32_t iris_files_allocated_bytes(void) { return 0; }
uint32_t iris_files_static_bytes(void) { return 0; }
#if !CONFIG_ESP_IRIS_SYSTEM_UPDATE
uint32_t iris_system_update_static_bytes(void) { return 0; }
#endif
esp_err_t esp_iris_platform_mark_planned_restart(void) { return ESP_OK; }
esp_err_t esp_iris_platform_mark_healthy(void) { return ESP_OK; }

static uint8_t incoming[ESP_IRIS_MAX_WIRE_FRAME_SIZE * 2],
               outgoing[ESP_IRIS_MAX_WIRE_FRAME_SIZE * 2];
static size_t incoming_size, incoming_offset, read_limit = 256;
static size_t outgoing_size, write_limit = 1024;
static bool candidate;
static void (*transport_poll_hook)(iris_runtime_t *runtime);
static unsigned starts, stops;
static esp_err_t fake_start(iris_runtime_t *r, iris_transport_state_t *s) { ++starts; s->driver_started = true; return ESP_OK; }
static void fake_stop(iris_runtime_t *r, iris_transport_state_t *s) { ++stops; s->driver_started = false; }
static iris_link_event_t fake_poll(iris_runtime_t *r, iris_transport_state_t *s) {
    if (transport_poll_hook != NULL && r != &g_iris_data) transport_poll_hook(r);
    if (candidate) { candidate = false; return IRIS_LINK_EVENT_CONNECTED; }
    return IRIS_LINK_EVENT_NONE;
}
static void fake_disconnect(iris_runtime_t *r, iris_transport_state_t *s) { }
static int fake_read(iris_runtime_t *r, iris_transport_state_t *s, uint8_t *b, size_t n) {
    if (n > incoming_size - incoming_offset) n = incoming_size - incoming_offset;
    if (n > read_limit) n = read_limit;
    memcpy(b, incoming + incoming_offset, n); incoming_offset += n; return (int)n;
}
static int fake_write(iris_runtime_t *r, iris_transport_state_t *s, const uint8_t *b, size_t n) {
    if (n > write_limit) n = write_limit;
    assert(outgoing_size + n <= sizeof(outgoing));
    memcpy(outgoing + outgoing_size, b, n); outgoing_size += n; return (int)n;
}
const iris_transport_ops_t g_iris_tcp_transport_ops = {
    ESP_IRIS_TRANSPORT_KIND_TCP, "tcp", fake_start, fake_stop,
    fake_poll, fake_disconnect, fake_read, fake_write };
#if CONFIG_ESP_IRIS_TRANSPORT_USB
const iris_transport_ops_t g_iris_usb_transport_ops = {
    ESP_IRIS_TRANSPORT_KIND_USB, "usb", fake_start, fake_stop,
    fake_poll, fake_disconnect, fake_read, fake_write };
#endif

#if CONFIG_ESP_IRIS_OTA
static const esp_partition_t running_partition = {0x10000, 4096, "running", 0};
static const esp_partition_t target_partition = {0x20000, 4096, "main_app_slot_00", 0};
static unsigned flash_writes, boot_selections, flash_aborts;
static unsigned ota_commits;
static esp_err_t ota_begin_error, ota_end_error, boot_select_error;
static size_t ota_begin_size;
static bool ota_handle_open;
static void (*during_ota_begin)(void);
static void (*during_ota_commit)(void);
static void (*during_flash)(void);
esp_partition_iterator_t esp_partition_find(unsigned t, unsigned s, const char *l) { return (void *)&target_partition; }
const esp_partition_t *esp_partition_get(esp_partition_iterator_t i) { return i; }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t i) { return NULL; }
void esp_partition_iterator_release(esp_partition_iterator_t i) { }
const esp_partition_t *esp_ota_get_running_partition(void) { return &running_partition; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { return &target_partition; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) {
    assert(!ota_handle_open);
    ota_begin_size = n; ota_handle_open = true; *h = 1;
    if (during_ota_begin) during_ota_begin();
    return ota_begin_error;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *b, size_t n) { ++flash_writes; if (during_flash) during_flash(); return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { ota_handle_open = false; if (during_flash) during_flash(); return ota_end_error; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { ota_handle_open = false; ++flash_aborts; return ESP_OK; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d) { memset(d, 0, sizeof(*d)); return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { ++boot_selections; return boot_select_error; }
void esp_iris_platform_ota_committed(void) {
    assert(boot_selections && boot_select_error == ESP_OK);
    ++ota_commits;
    if (during_ota_commit) during_ota_commit();
}
esp_err_t esp_iris_platform_select_ota_target(uint32_t candidate, uint32_t *out) { *out = candidate; return ESP_OK; }
esp_err_t esp_iris_platform_prepare_ota(uint32_t a, uint32_t b) { return ESP_OK; }
#endif

void iris_log_console_frame_begin(iris_runtime_t *r) { }
void iris_log_console_frame_end(iris_runtime_t *r) { }
bool iris_log_uses_native_console(const iris_runtime_t *r) { return false; }
void iris_log_forward_deferred(const iris_log_record_t *r) { }
