#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_iris_codec.h"
#include "esp_iris_console_codec.h"
#include "esp_iris_state.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define IRIS_LOG_RECORD_DATA_MAX 240U
#define IRIS_LOG_RECORD_HEADER_SIZE 12U
#define IRIS_LOG_DEFERRED_CONSOLE 1U

typedef enum {
    IRIS_LINK_EVENT_NONE,
    IRIS_LINK_EVENT_CONNECTED,
    IRIS_LINK_EVENT_DISCONNECTED,
} iris_link_event_t;

typedef struct {
    int listen_fd;
    int client_fd;
    bool link_up;
    bool reported_link_up;
    bool driver_started;
    bool driver_owned;
    bool pending_rx_valid;
    uint8_t pending_rx;
    atomic_bool host_open;
    atomic_bool disconnect_pending;
    int usb_serial_jtag_install_core;
} iris_transport_state_t;

struct iris_runtime;

typedef struct {
    esp_iris_transport_kind_t kind;
    const char *name;
    esp_err_t (*start)(struct iris_runtime *runtime,
                       iris_transport_state_t *state);
    void (*stop)(struct iris_runtime *runtime, iris_transport_state_t *state);
    iris_link_event_t (*poll)(struct iris_runtime *runtime,
                              iris_transport_state_t *state);
    void (*disconnect)(struct iris_runtime *runtime,
                       iris_transport_state_t *state);
    int (*read)(struct iris_runtime *runtime, iris_transport_state_t *state,
                uint8_t *buffer, size_t capacity);
    int (*write)(struct iris_runtime *runtime, iris_transport_state_t *state,
                 const uint8_t *buffer, size_t length);
} iris_transport_ops_t;

typedef struct {
    iris_transport_state_t usb;
    iris_transport_state_t tcp;
    iris_transport_state_t usb_serial_jtag;
    iris_transport_state_t uart;
    const iris_transport_ops_t *active_ops;
    iris_transport_state_t *active_state;
    int64_t claim_deadline_us;
    int64_t retry_after_us[5];
    uint8_t scan_start;
    bool committed;
} iris_transport_manager_t;

typedef struct {
    uint64_t monotonic_us;
    uint32_t dropped_total;
    uint8_t source;
    uint8_t flags;
    uint16_t length;
    uint8_t data[IRIS_LOG_RECORD_DATA_MAX];
} iris_log_record_t;

typedef struct iris_runtime {
    bool data_link;
    bool started;
    bool initializing;
    bool running;
    bool identity_ready;
    bool crash_initialized;
    bool crash_loop_initialized;
    bool vfs_registered;
    bool stdio_redirected;
    bool link_connected;
    bool hello_acked;
    bool healthy;
    bool previous_boot_crash;
    bool previous_boot_planned;
    bool core_dump_present;
    bool core_dump_valid;
    bool core_dump_checked;
    bool crash_loop_triggered;
    bool crash_recovery_pending;
    esp_iris_lifecycle_t lifecycle;
    iris_session_state_t session_state;

    uint8_t device_id[16];
    uint8_t hardware_mac[6];
    uint64_t boot_id;
    uint32_t session_id;
    uint8_t owner_id[16];
    uint8_t auth_challenge[32];
    int64_t auth_retry_after_us;
    uint32_t last_rpc_request_id;
    bool rpc_request_seen;
    uint32_t sequence[ESP_IRIS_CHANNEL_COUNT];
    uint32_t rx_sequence[ESP_IRIS_CHANNEL_COUNT];
    bool rx_sequence_seen[ESP_IRIS_CHANNEL_COUNT];
    uint32_t log_credit;
    int64_t next_hello_us;
    int64_t crash_stable_deadline_us;
    uint32_t pending_events;

    TaskHandle_t task;
    iris_transport_manager_t transport;

    uint8_t rx_pending[256];
    size_t rx_pending_length;
    size_t rx_pending_offset;

#ifdef CONFIG_ESP_IRIS_WIRE_BUFFERS_PSRAM
    uint8_t *rx_wire;
#else
    uint8_t rx_wire[IRIS_CONSOLE_RECORD_BYTES];
#endif
    size_t rx_wire_length;
    bool rx_discarding;
    bool disconnect_after_tx;

#ifdef CONFIG_ESP_IRIS_WIRE_BUFFERS_PSRAM
    uint8_t *tx_wire;
#else
    uint8_t tx_wire[IRIS_CONSOLE_RECORD_BYTES];
#endif
    size_t tx_wire_length;
    size_t tx_wire_offset;

    portMUX_TYPE log_lock;
    portMUX_TYPE event_lock;
    size_t log_tail;
    size_t log_used;
    uint32_t log_dropped_bytes;
    uint32_t log_dropped_reported;
    bool retain_usb_logs;

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t invalid_frames;
    uint32_t link_count;
    uint32_t task_stack_free_min_bytes;
    uint32_t worker_active_max_us;
    uint32_t internal_heap_used_bytes;
    uint32_t service_bytes_at_start;

    const esp_partition_t *core_dump_partition;
    size_t core_dump_address;
    size_t core_dump_size;
    char core_dump_elf_sha256[65];
    uint8_t core_dump_elf_sha256_length;
    char panic_reason[128];
    uint32_t crash_count;
    uint32_t crash_limit;
    uint32_t crash_origin_reset_reason;
    uint32_t crash_failed_app_address;
    uint64_t crash_failed_boot_id;
    uint8_t crash_failed_firmware_sha256[32];
    esp_err_t crash_state_error;
} iris_runtime_t;


/* Slow services own business bytes only. Protocol/session sequencing, framing
 * and transports belong exclusively to the protocol task. */
typedef struct {
    uint8_t *payload;
    size_t capacity;
    esp_iris_wire_header_t response;
    bool ready;
} iris_service_call_t;

static inline esp_err_t iris_service_reply(
    iris_service_call_t *call, uint8_t channel, uint8_t type, uint16_t flags,
    uint32_t request_id, uint32_t stream_id,
    const uint8_t *payload, size_t payload_size)
{
    if (call->ready) return ESP_ERR_INVALID_STATE;
    if (payload_size > call->capacity ||
        payload_size > ESP_IRIS_MAX_PAYLOAD_SIZE) return ESP_ERR_INVALID_SIZE;
    if (payload_size != 0) memmove(call->payload, payload, payload_size);
    call->response = (esp_iris_wire_header_t){
        .channel = channel, .type = type, .flags = flags,
        .request_id = request_id, .stream_id = stream_id,
        .payload_size = payload_size,
    };
    call->ready = true;
    return ESP_OK;
}

static inline esp_err_t iris_service_error(
    iris_service_call_t *call, uint32_t request_id, esp_err_t code,
    uint8_t channel, uint8_t type)
{
    uint8_t payload[8] = {0};
    iris_put_le32(payload, (uint32_t)code);
    payload[4] = channel;
    payload[5] = type;
    return iris_service_reply(call, ESP_IRIS_CHANNEL_CONTROL,
        ESP_IRIS_CONTROL_ERROR, ESP_IRIS_FLAG_RESPONSE | ESP_IRIS_FLAG_ERROR,
        request_id, 0, payload, sizeof(payload));
}

extern iris_runtime_t g_iris;
extern iris_runtime_t g_iris_data;
/* Both links are scheduled by one protocol task. These helpers are task-owned. */
iris_runtime_t *iris_peer_runtime(const iris_runtime_t *runtime);
bool iris_session_is_live(uint32_t session_id);
esp_err_t iris_link_claim(iris_runtime_t *runtime, const uint8_t owner_id[16]);
esp_iris_transport_kind_t iris_runtime_transport_kind(const iris_runtime_t *runtime);
esp_err_t iris_runtime_wire_init(iris_runtime_t *runtime);
void iris_runtime_wire_deinit(iris_runtime_t *runtime);
void iris_notify_worker(iris_runtime_t *runtime);
void iris_crash_context_prepare(iris_runtime_t *runtime);

esp_err_t iris_identity_load_or_create(iris_runtime_t *runtime);

esp_err_t iris_log_vfs_init(iris_runtime_t *runtime);
esp_err_t iris_log_capture_start(iris_runtime_t *runtime);
esp_err_t iris_log_capture_stop(iris_runtime_t *runtime);
esp_err_t iris_log_vfs_deinit(void);
esp_err_t iris_log_redirect_stdio(void);
esp_err_t iris_log_restore_stdio(void);
void iris_log_console_frame_begin(iris_runtime_t *runtime);
void iris_log_console_frame_end(iris_runtime_t *runtime);
bool iris_log_uses_native_console(const iris_runtime_t *runtime);
void iris_log_forward_deferred(const iris_log_record_t *record);
bool iris_log_pop(iris_runtime_t *runtime, size_t payload_budget,
                  iris_log_record_t *out_record);

void iris_crash_probe(iris_runtime_t *runtime);
esp_err_t iris_crash_recovery_probe(iris_runtime_t *runtime);
esp_err_t iris_crash_recovery_mark_planned(iris_runtime_t *runtime);
esp_err_t iris_crash_recovery_mark_healthy(iris_runtime_t *runtime);
esp_err_t iris_crash_recovery_reset(iris_runtime_t *runtime);
void iris_crash_recovery_poll(iris_runtime_t *runtime, int64_t now_us);
esp_err_t iris_crash_build_metadata(iris_runtime_t *runtime, uint8_t *out,
                                    size_t capacity, size_t *out_size);
esp_err_t iris_crash_read(iris_runtime_t *runtime, size_t offset,
                          size_t maximum, uint8_t *out, size_t *out_size);

esp_err_t iris_files_init(iris_runtime_t *runtime);
void iris_files_deinit(void);
void iris_files_session_end(uint32_t session_id);
bool iris_files_handle_frame(iris_runtime_t *runtime,
                             const iris_decoded_frame_t *frame);
bool iris_files_queue_next(iris_runtime_t *runtime);
uint64_t iris_files_capabilities(void);
uint32_t iris_files_allocated_bytes(void);
uint32_t iris_files_static_bytes(void);

void iris_system_update_session_end(void);
esp_err_t iris_system_update_request_cancel(const uint8_t *operation_id, size_t size);
void iris_system_update_cancel_all(void);
bool iris_system_update_cancel_pending(void);
void iris_system_update_poll_cancel(void);
bool iris_system_inventory_handle_frame(iris_service_call_t *runtime,
                                        const iris_decoded_frame_t *frame);
uint64_t iris_system_inventory_capabilities(void);
uint32_t iris_system_inventory_static_bytes(void);
bool iris_system_inventory_registered(void);
bool iris_system_update_handle_frame(iris_service_call_t *runtime,
                                     const iris_decoded_frame_t *frame);
uint64_t iris_system_update_capabilities(void);
uint32_t iris_system_update_static_bytes(void);
bool iris_system_update_backend_registered(void);

esp_err_t iris_queue_frame(iris_runtime_t *runtime, uint8_t channel,
                           uint8_t type, uint16_t flags,
                           uint32_t request_id, uint32_t stream_id,
                           const uint8_t *payload, size_t payload_size);
esp_err_t iris_queue_error(iris_runtime_t *runtime, uint32_t request_id,
                           esp_err_t code, uint8_t channel, uint8_t type);

esp_err_t iris_services_init(iris_runtime_t *runtime);
void iris_services_deinit(iris_runtime_t *runtime);
void iris_services_session_begin(iris_runtime_t *runtime);
void iris_services_session_end(iris_runtime_t *runtime);
bool iris_services_handle_frame(iris_runtime_t *runtime,
                                const iris_decoded_frame_t *frame,
                                uint64_t received_us);
bool iris_services_queue_next(iris_runtime_t *runtime);
bool iris_services_work_pending(void);
void iris_services_poll(iris_runtime_t *runtime);
uint64_t iris_services_capabilities(const iris_runtime_t *runtime);
uint8_t iris_services_auth_mode(const iris_runtime_t *runtime);
const uint8_t *iris_services_auth_challenge(const iris_runtime_t *runtime, size_t *size);
esp_err_t iris_services_authenticate(iris_runtime_t *runtime,
                                     const uint8_t *payload, size_t size);
bool iris_services_credit(iris_runtime_t *runtime, uint8_t channel,
                          uint32_t stream_id, uint32_t amount);
uint32_t iris_services_allocated_bytes(void);
uint32_t iris_services_static_bytes(void);

esp_err_t iris_transport_start(iris_runtime_t *runtime);
void iris_transport_stop(iris_runtime_t *runtime);
iris_link_event_t iris_transport_poll(iris_runtime_t *runtime);
int iris_transport_read(iris_runtime_t *runtime, uint8_t *buffer,
                        size_t capacity);
int iris_transport_write(iris_runtime_t *runtime, const uint8_t *buffer,
                         size_t length);
esp_iris_transport_kind_t iris_transport_kind(void);
const char *iris_transport_name(void);
void iris_transport_commit(iris_runtime_t *runtime);
void iris_transport_renew_claim(iris_runtime_t *runtime);
void iris_transport_disconnect(iris_runtime_t *runtime);

#if CONFIG_ESP_IRIS_TRANSPORT_USB
extern const iris_transport_ops_t g_iris_usb_transport_ops;
void iris_usb_console_wait(void);
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_TCP
extern const iris_transport_ops_t g_iris_tcp_transport_ops;
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG
extern const iris_transport_ops_t g_iris_usb_serial_jtag_transport_ops;
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_UART
extern const iris_transport_ops_t g_iris_uart_transport_ops;
#endif

#if CONFIG_ESP_IRIS_CONSOLE_EXTERNAL_INPUT
void iris_console_input_enable(bool enabled);
bool iris_console_input_available(void);
int iris_console_input_read(uint8_t *buffer, size_t capacity);
#endif
