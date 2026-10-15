#include "esp_iris_internal.h"
#include "esp_iris_memory.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#define IRIS_HELLO_INTERVAL_US (1000LL * 1000LL)
#define IRIS_LOG_PAYLOAD_HEADER_SIZE 16U
#define IRIS_EVENT_BIT(type) (1UL << (uint32_t)(type))
#define IRIS_STDIO_STATIC_BYTES (512U + 4U * sizeof(void *))
#define IRIS_STOP_TIMEOUT_MS 1000U
#define IRIS_MAX_TASK_MEMORY_RECORDS 128U
#define IRIS_WORKER_RUN_BUDGET_US (20LL * 1000LL)

iris_runtime_t g_iris;

#if CONFIG_ESP_IRIS_DATA_LINK
iris_runtime_t g_iris_data;
#endif

/* Keep the large, mostly-zero runtimes in BSS. Run before ordinary application
 * constructors, not on esp_iris_start(): callers can use status/log/boot APIs
 * before starting a transport, and stop/start must preserve runtime state. */
static void __attribute__((constructor(101))) iris_runtime_boot_init(void)
{
    g_iris.transport.tcp.listen_fd = -1;
    g_iris.transport.tcp.client_fd = -1;
    g_iris.log_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g_iris.event_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g_iris.lifecycle = ESP_IRIS_LIFECYCLE_STOPPED;
    g_iris.task_stack_free_min_bytes = UINT32_MAX;
#if CONFIG_ESP_IRIS_DATA_LINK
    g_iris_data.data_link = true;
    g_iris_data.transport.tcp.listen_fd = -1;
    g_iris_data.transport.tcp.client_fd = -1;
    g_iris_data.log_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g_iris_data.event_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
#endif
}

iris_runtime_t *iris_peer_runtime(const iris_runtime_t *runtime)
{
#if CONFIG_ESP_IRIS_DATA_LINK
    return runtime->data_link ? &g_iris : &g_iris_data;
#else
    (void)runtime;
    return NULL;
#endif
}

bool iris_session_is_live(uint32_t session_id)
{
    return session_id != 0 && ((g_iris.hello_acked && g_iris.session_id == session_id)
#if CONFIG_ESP_IRIS_DATA_LINK
        || (g_iris_data.hello_acked && g_iris_data.session_id == session_id)
#endif
    );
}

static iris_runtime_t *common_runtime(iris_runtime_t *runtime)
{
#if CONFIG_ESP_IRIS_DATA_LINK
    if (runtime == &g_iris_data) return &g_iris;
#endif
    return runtime;
}

static uint32_t runtime_static_bytes(void)
{
    return sizeof(g_iris) + IRIS_STDIO_STATIC_BYTES + iris_services_static_bytes()
#if CONFIG_ESP_IRIS_DATA_LINK
        + sizeof(g_iris_data)
#endif
#if CONFIG_ESP_IRIS_CONSOLE_EXTERNAL_INPUT
        + IRIS_CONSOLE_RECORD_BYTES + sizeof(portMUX_TYPE) + 2 * sizeof(size_t) + sizeof(bool)
#endif
        ;
}

esp_err_t iris_link_claim(iris_runtime_t *runtime, const uint8_t owner_id[16])
{
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 16; ++i) nonzero |= owner_id[i];
    if (!nonzero) return ESP_ERR_INVALID_ARG;
    const iris_runtime_t *peer = iris_peer_runtime(runtime);
    if (peer != NULL && peer->hello_acked && memcmp(peer->owner_id, owner_id, 16) != 0)
        return ESP_ERR_INVALID_STATE;
    memcpy(runtime->owner_id, owner_id, 16);
    return ESP_OK;
}

static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t iris_runtime_wire_init(iris_runtime_t *runtime)
{
#ifdef CONFIG_ESP_IRIS_WIRE_BUFFERS_PSRAM
    if (runtime->rx_wire != NULL) {
        return ESP_OK;
    }
    uint8_t *frames = heap_caps_malloc(2U * IRIS_CONSOLE_RECORD_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frames == NULL) {
        return ESP_ERR_NO_MEM;
    }
    runtime->rx_wire = frames;
    runtime->tx_wire = frames + IRIS_CONSOLE_RECORD_BYTES;
#else
    (void)runtime;
#endif
    return ESP_OK;
}

void iris_runtime_wire_deinit(iris_runtime_t *runtime)
{
#ifdef CONFIG_ESP_IRIS_WIRE_BUFFERS_PSRAM
    heap_caps_free(runtime->rx_wire);
    runtime->rx_wire = NULL;
    runtime->tx_wire = NULL;
#else
    (void)runtime;
#endif
}

void iris_notify_worker(iris_runtime_t *runtime)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (runtime->task != NULL) xTaskNotifyGive(runtime->task);
    taskEXIT_CRITICAL(&s_start_lock);
}

static bool transition_lifecycle(iris_runtime_t *runtime,
                                 esp_iris_lifecycle_t requested)
{
    return iris_lifecycle_transition(runtime->lifecycle, requested,
                                     &runtime->lifecycle);
}

static bool transition_session(iris_runtime_t *runtime,
                               iris_session_event_t event)
{
    iris_session_state_t next = runtime->session_state;
    if (!iris_session_transition(runtime->session_state, event, &next)) {
        return false;
    }
    runtime->session_state = next;
    runtime->link_connected = next != IRIS_SESSION_DISCONNECTED;
    runtime->hello_acked = next == IRIS_SESSION_READY;
    return true;
}

#include "esp_iris_control_frames.inc"

static void begin_session(iris_runtime_t *runtime);
static void end_session(iris_runtime_t *runtime);

static void handle_control(iris_runtime_t *runtime,
                           const iris_decoded_frame_t *frame,
                           uint64_t received_us)
{
    const esp_iris_wire_header_t *header = &frame->header;
    switch (header->type) {
    case ESP_IRIS_CONTROL_HELLO_ACK:
        if (header->flags & ESP_IRIS_FLAG_NEW_SESSION) {
            if (iris_services_authenticate(runtime, frame->payload,
                                           header->payload_size) != ESP_OK) {
                (void)queue_error(runtime, header->request_id,
                                  ESP_ERR_INVALID_STATE, header->channel,
                                  header->type);
                return;
            }
            end_session(runtime);
            begin_session(runtime);
            iris_transport_renew_claim(runtime);
            (void)queue_hello(runtime);
            runtime->next_hello_us = esp_timer_get_time() + IRIS_HELLO_INTERVAL_US;
            return;
        }
        if (runtime->hello_acked) {
            break;
        }
        {
            esp_err_t auth_err = iris_services_authenticate(
                runtime, frame->payload, header->payload_size);
            { /* Every link receives explicit authenticated binding acceptance. */
                const uint8_t result = auth_err == ESP_OK ? 1U : 0U;
                (void)queue_frame(
                    runtime, ESP_IRIS_CHANNEL_CONTROL,
                    ESP_IRIS_CONTROL_AUTH_RESULT,
                    ESP_IRIS_FLAG_RESPONSE |
                        (auth_err == ESP_OK ? 0 : ESP_IRIS_FLAG_ERROR),
                    header->request_id, 0, &result, sizeof(result));
                if (auth_err != ESP_OK) {
                    runtime->disconnect_after_tx = true;
                }
            }
            if (auth_err == ESP_OK && transition_session(
                    runtime, IRIS_SESSION_EVENT_AUTHENTICATED)) {
                schedule_session_events(runtime);
                iris_transport_commit(runtime);
            }
        }
        break;
    case ESP_IRIS_CONTROL_PING:
        (void)queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                          ESP_IRIS_CONTROL_PONG, ESP_IRIS_FLAG_RESPONSE,
                          header->request_id, 0, frame->payload,
                          header->payload_size);
        break;
    case ESP_IRIS_CONTROL_TIME_SYNC_REQUEST:
        if (header->payload_size == 8U) {
            uint8_t payload[24];
            memcpy(payload, frame->payload, 8);
            iris_put_le64(payload + 8, received_us);
            iris_put_le64(payload + 16, (uint64_t)esp_timer_get_time());
            (void)queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                              ESP_IRIS_CONTROL_TIME_SYNC_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE, header->request_id, 0,
                              payload, sizeof(payload));
        } else {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
        }
        break;
    case ESP_IRIS_CONTROL_STATUS_REQUEST:
        (void)queue_status(runtime, header->request_id);
        break;
    case ESP_IRIS_CONTROL_TASKS_REQUEST:
        if (header->payload_size != 0) {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel, header->type);
            break;
        }
        {
            esp_err_t err = queue_task_memory(runtime, header->request_id);
            if (err != ESP_OK) {
                (void)queue_error(runtime, header->request_id, err,
                                  header->channel, header->type);
            }
        }
        break;
    case ESP_IRIS_CONTROL_CREDIT:
        if (header->payload_size == 8U &&
                frame->payload[0] == ESP_IRIS_CHANNEL_LOG) {
            const uint32_t amount = iris_get_le32(frame->payload + 4);
            runtime->log_credit = UINT32_MAX - runtime->log_credit < amount
                ? UINT32_MAX : runtime->log_credit + amount;
        } else if (header->payload_size == 8U &&
                   iris_services_credit(runtime, frame->payload[0], header->stream_id,
                                        iris_get_le32(frame->payload + 4))) {
            /* Media credits are maintained independently per channel. */
        } else {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_ARG, header->channel,
                              header->type);
        }
        break;
    default:
        (void)queue_error(runtime, header->request_id,
                          ESP_ERR_NOT_SUPPORTED, header->channel,
                          header->type);
        break;
    }
}

static void handle_crash(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame)
{
    iris_runtime_t *common = common_runtime(runtime);
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->type == ESP_IRIS_CRASH_METADATA_REQUEST) {
        if (header->payload_size != 0) {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
            return;
        }
        size_t payload_size = 0;
        esp_err_t err = iris_crash_build_metadata(
            common, runtime->rx_wire, ESP_IRIS_MAX_WIRE_FRAME_SIZE,
            &payload_size);
        if (err == ESP_OK) {
            err = queue_frame(runtime, ESP_IRIS_CHANNEL_CRASH,
                              ESP_IRIS_CRASH_METADATA_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE, header->request_id, 0,
                              runtime->rx_wire, payload_size);
        }
        if (err != ESP_OK) {
            (void)queue_error(runtime, header->request_id, err,
                              header->channel, header->type);
        }
        return;
    }
    if (header->type == ESP_IRIS_CRASH_READ_REQUEST) {
        if (header->payload_size != 8U) {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
            return;
        }
        const uint32_t offset = iris_get_le32(frame->payload);
        const uint16_t maximum = iris_get_le16(frame->payload + 4);
        size_t chunk_size = 0;
        esp_err_t err = iris_crash_read(common, offset, maximum,
                                        runtime->rx_wire + 8,
                                        &chunk_size);
        if (err == ESP_OK) {
            iris_put_le32(runtime->rx_wire, offset);
            iris_put_le32(runtime->rx_wire + 4,
                          common->core_dump_size <= UINT32_MAX
                            ? (uint32_t)common->core_dump_size
                            : UINT32_MAX);
            const bool finished = offset + chunk_size >=
                                  common->core_dump_size;
            err = queue_frame(runtime, ESP_IRIS_CHANNEL_CRASH,
                              ESP_IRIS_CRASH_READ_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE |
                                (finished ? ESP_IRIS_FLAG_STREAM_END : 0),
                              header->request_id, 1,
                              runtime->rx_wire, chunk_size + 8U);
        }
        if (err != ESP_OK) {
            (void)queue_error(runtime, header->request_id, err,
                              header->channel, header->type);
        }
        return;
    }
    (void)queue_error(runtime, header->request_id, ESP_ERR_NOT_SUPPORTED,
                      header->channel, header->type);
}

static void handle_frame(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame,
                         uint64_t received_us)
{
    ++runtime->rx_frames;
    if (frame->header.session_id != runtime->session_id) {
        ++runtime->invalid_frames;
        return;
    }
    if (!runtime->hello_acked &&
        !(frame->header.channel == ESP_IRIS_CHANNEL_CONTROL &&
          frame->header.type == ESP_IRIS_CONTROL_HELLO_ACK)) {
        ++runtime->invalid_frames;
        return;
    }
    /* HELLO_ACK is idempotent; NEW_SESSION authenticates before resetting.
     * All other frames use RFC 1982-style uint32 serial arithmetic. */
    const uint8_t channel = frame->header.channel;
    if (!(channel == ESP_IRIS_CHANNEL_CONTROL &&
          frame->header.type == ESP_IRIS_CONTROL_HELLO_ACK)) {
        const uint32_t distance = frame->header.sequence - runtime->rx_sequence[channel];
        if (runtime->rx_sequence_seen[channel] &&
                (distance == 0 || distance >= UINT32_C(0x80000000))) {
            ++runtime->invalid_frames;
            return;
        }
        runtime->rx_sequence[channel] = frame->header.sequence;
        runtime->rx_sequence_seen[channel] = true;
    }
    if (frame->header.channel == ESP_IRIS_CHANNEL_CONTROL &&
        (frame->header.type == ESP_IRIS_CONTROL_HELLO_ACK ||
         frame->header.type == ESP_IRIS_CONTROL_PING ||
         frame->header.type == ESP_IRIS_CONTROL_TIME_SYNC_REQUEST ||
         frame->header.type == ESP_IRIS_CONTROL_STATUS_REQUEST ||
         frame->header.type == ESP_IRIS_CONTROL_TASKS_REQUEST ||
         frame->header.type == ESP_IRIS_CONTROL_CREDIT)) {
        handle_control(runtime, frame, received_us);
    } else if (frame->header.channel == ESP_IRIS_CHANNEL_CRASH) {
        handle_crash(runtime, frame);
    } else if (iris_services_handle_frame(runtime, frame, received_us)) {
        return;
    } else {
        (void)queue_error(runtime, frame->header.request_id,
                          ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                          frame->header.type);
    }
}

#include "esp_iris_console_dispatch.inc"

static size_t feed_rx(iris_runtime_t *runtime, const uint8_t *data,
                    size_t length)
{
    if (!runtime->data_link) return feed_console(runtime, data, length);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = data[i];
        if (value == 0) {
            if (runtime->rx_discarding) {
                runtime->rx_discarding = false;
                runtime->rx_wire_length = 0;
                continue;
            }
            if (runtime->rx_wire_length == 0) {
                continue;
            }
            iris_decoded_frame_t frame;
            const uint64_t received_us = (uint64_t)esp_timer_get_time();
            if (iris_frame_decode_in_place(runtime->rx_wire,
                                           runtime->rx_wire_length,
                                           &frame) == ESP_OK) {
                handle_frame(runtime, &frame, received_us);
            } else {
                ++runtime->invalid_frames;
            }
            runtime->rx_wire_length = 0;
            if (runtime->tx_wire_length != 0) {
                return i + 1U;
            }
            continue;
        }
        if (runtime->rx_discarding) {
            continue;
        }
        if (runtime->rx_wire_length >= ESP_IRIS_MAX_WIRE_FRAME_SIZE - 1U) {
            runtime->rx_discarding = true;
            runtime->rx_wire_length = 0;
            ++runtime->invalid_frames;
            continue;
        }
        runtime->rx_wire[runtime->rx_wire_length++] = value;
    }
    return length;
}

static void begin_session(iris_runtime_t *runtime)
{
    if (!transition_session(runtime, IRIS_SESSION_EVENT_LINK_UP)) {
        return;
    }
    const iris_runtime_t *peer = iris_peer_runtime(runtime);
    do {
        runtime->session_id = esp_random();
    } while (runtime->session_id == 0 ||
             (peer != NULL && runtime->session_id == peer->session_id));
    memset(runtime->sequence, 0, sizeof(runtime->sequence));
    memset(runtime->rx_sequence_seen, 0, sizeof(runtime->rx_sequence_seen));
    taskENTER_CRITICAL(&runtime->event_lock);
    runtime->pending_events = 0;
    taskEXIT_CRITICAL(&runtime->event_lock);
    runtime->log_credit = 0;
    runtime->last_rpc_request_id = 0;
    runtime->rpc_request_seen = false;
    memset(runtime->owner_id, 0, sizeof(runtime->owner_id));
    esp_fill_random(runtime->auth_challenge, sizeof(runtime->auth_challenge));
    runtime->next_hello_us = 0;
    runtime->rx_wire_length = 0;
    runtime->rx_pending_length = 0;
    runtime->rx_pending_offset = 0;
    runtime->rx_discarding = false;
    runtime->disconnect_after_tx = false;
    runtime->tx_wire_length = 0;
    runtime->tx_wire_offset = 0;
    ++runtime->link_count;
    iris_services_session_begin(runtime);
}

static void end_session(iris_runtime_t *runtime)
{
    iris_log_console_frame_end(runtime);
    if (!runtime->data_link) {
        iris_log_record_t record;
        while (iris_log_pop(runtime, UINT32_MAX, &record))
            iris_log_forward_deferred(&record);
    }
    /* A provisional multi-transport candidate cannot reach services before a
     * valid HELLO_ACK. Do not let a handshake timeout cancel product jobs or
     * tear down media/file state that no client was allowed to create. */
    if (runtime->hello_acked) {
        iris_services_session_end(runtime);
    }
    (void)transition_session(runtime, IRIS_SESSION_EVENT_LINK_DOWN);
    taskENTER_CRITICAL(&runtime->event_lock);
    runtime->pending_events = 0;
    taskEXIT_CRITICAL(&runtime->event_lock);
    runtime->session_id = 0;
    memset(runtime->owner_id, 0, sizeof(runtime->owner_id));
    runtime->log_credit = 0;
    runtime->rx_wire_length = 0;
    runtime->rx_pending_length = 0;
    runtime->rx_pending_offset = 0;
    runtime->rx_discarding = false;
    runtime->disconnect_after_tx = false;
    runtime->tx_wire_length = 0;
    runtime->tx_wire_offset = 0;
}

static bool flush_tx(iris_runtime_t *runtime)
{
    if (runtime->tx_wire_length == 0) {
        return false;
    }
    if (runtime->tx_wire_offset == 0) iris_log_console_frame_begin(runtime);
    const int sent = iris_transport_write(
        runtime, runtime->tx_wire + runtime->tx_wire_offset,
        runtime->tx_wire_length - runtime->tx_wire_offset);
    if (sent <= 0) {
        return false;
    }
    runtime->tx_wire_offset += (size_t)sent;
    if (runtime->tx_wire_offset == runtime->tx_wire_length) {
        runtime->tx_wire_length = 0;
        runtime->tx_wire_offset = 0;
        ++runtime->tx_frames;
        iris_log_console_frame_end(runtime);
    }
    return true;
}

static void queue_next_log(iris_runtime_t *runtime)
{
    if (runtime->data_link && runtime->log_credit < IRIS_LOG_PAYLOAD_HEADER_SIZE) {
        return;
    }
    iris_log_record_t record;
    if (!iris_log_pop(runtime, runtime->data_link ? runtime->log_credit
                                                : UINT32_MAX, &record)) {
        return;
    }
    if (!runtime->data_link) {
        /* Native UART/Serial-JTAG output is already forwarded by the log tap.
         * Network and application USB consoles receive the same plain bytes. */
        iris_log_forward_deferred(&record);
        if (!iris_log_uses_native_console(runtime)) {
            memcpy(runtime->tx_wire, record.data, record.length);
            runtime->tx_wire_length = record.length;
            runtime->tx_wire_offset = 0;
        }
        return;
    }
    uint8_t payload[IRIS_LOG_PAYLOAD_HEADER_SIZE + IRIS_LOG_RECORD_DATA_MAX];
    iris_put_le64(payload, record.monotonic_us);
    iris_put_le32(payload + 8, record.dropped_total);
    payload[12] = record.source;
    payload[13] = record.flags;
    iris_put_le16(payload + 14, record.length);
    memcpy(payload + IRIS_LOG_PAYLOAD_HEADER_SIZE, record.data, record.length);
    const size_t payload_size = IRIS_LOG_PAYLOAD_HEADER_SIZE + record.length;
    if (queue_frame(runtime, ESP_IRIS_CHANNEL_LOG, ESP_IRIS_LOG_RECORD, 0,
                    0, 0, payload, payload_size) == ESP_OK) {
        runtime->log_credit -= payload_size;
    }
}

static bool pump_link(iris_runtime_t *runtime)
{
    bool progressed = flush_tx(runtime);
    if (runtime->tx_wire_length == 0 && runtime->disconnect_after_tx) {
        runtime->disconnect_after_tx = false;
        iris_transport_disconnect(runtime);
        return true;
    }
    if (runtime->tx_wire_length != 0) {
        iris_services_poll(runtime);
        return progressed;
    }

    /* Preserve coalesced frame tails while the single TX slot drains. */
    if (runtime->rx_pending_offset == runtime->rx_pending_length) {
        runtime->rx_pending_offset = 0;
        runtime->rx_pending_length = 0;
        const int received = iris_transport_read(runtime, runtime->rx_pending,
                                                 sizeof(runtime->rx_pending));
        if (received > 0) {
            runtime->rx_pending_length = (size_t)received;
        }
    }
    if (runtime->rx_pending_offset < runtime->rx_pending_length) {
        const uint32_t input_session = runtime->session_id;
        const size_t consumed = feed_rx(
            runtime, runtime->rx_pending + runtime->rx_pending_offset,
            runtime->rx_pending_length - runtime->rx_pending_offset);
        if (runtime->session_id == input_session) {
            runtime->rx_pending_offset += consumed;
        }
        progressed = true;
    }

    if (runtime->tx_wire_length == 0) {
        const int64_t now = esp_timer_get_time();
        if (runtime->data_link && !runtime->hello_acked &&
                now >= runtime->next_hello_us) {
            if (queue_hello(runtime) == ESP_OK) {
                runtime->next_hello_us = now + IRIS_HELLO_INTERVAL_US;
            }
        } else if (runtime->hello_acked &&
                   next_pending_event(runtime) != 0) {
            (void)queue_event(runtime, next_pending_event(runtime));
        } else if (runtime->hello_acked &&
                   iris_services_queue_next(runtime)) {
            /* A service event or media chunk now owns TX. */
        } else if (runtime->hello_acked || !runtime->data_link) {
            if (!runtime->data_link) queue_next_log(runtime);
        }
    }

    if (runtime->tx_wire_length != 0) {
        progressed = flush_tx(runtime) || progressed;
    }
    if (runtime->tx_wire_length == 0 && runtime->disconnect_after_tx) {
        runtime->disconnect_after_tx = false;
        iris_transport_disconnect(runtime);
        return true;
    }
    iris_services_poll(runtime);
    return progressed;
}

static bool service_link(iris_runtime_t *runtime)
{
    const iris_link_event_t event = iris_transport_poll(runtime);
    if (event == IRIS_LINK_EVENT_CONNECTED) begin_session(runtime);
    else if (event == IRIS_LINK_EVENT_DISCONNECTED) end_session(runtime);
    bool progressed = false;
    for (size_t burst = 0; runtime->link_connected && burst < 8; ++burst) {
        const bool step = pump_link(runtime);
        progressed |= step;
        if (!step || runtime->tx_wire_length != 0) break;
    }
    return progressed;
}

static void iris_worker(void *argument)
{
    iris_runtime_t *runtime = argument;
    const TickType_t idle_ticks = pdMS_TO_TICKS(10) > 0
        ? pdMS_TO_TICKS(10) : 1;
    int64_t yield_deadline_us = esp_timer_get_time() + IRIS_WORKER_RUN_BUDGET_US;
    while (runtime->running) {
        const int64_t active_start_us = esp_timer_get_time();
        iris_crash_recovery_poll(runtime, active_start_us);
        iris_services_poll(runtime);
        bool progressed = service_link(runtime);
#if CONFIG_ESP_IRIS_DATA_LINK
        g_iris_data.task = runtime->task;
        progressed |= service_link(&g_iris_data);
#endif
        const int64_t active_time_us = esp_timer_get_time() - active_start_us;
        if (active_time_us > 0 &&
                (uint64_t)active_time_us > runtime->worker_active_max_us) {
            runtime->worker_active_max_us = active_time_us > UINT32_MAX
                ? UINT32_MAX : (uint32_t)active_time_us;
        }
        const uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark2(NULL);
        if (stack_free < runtime->task_stack_free_min_bytes) {
            runtime->task_stack_free_min_bytes = stack_free;
        }
        const bool tx_blocked = runtime->link_connected &&
            runtime->tx_wire_length != 0 && !progressed;
        if (tx_blocked || esp_timer_get_time() >= yield_deadline_us) {
            /* taskYIELD() cannot schedule lower-priority tasks, including
             * Idle. Really block on TX backpressure and periodically during
             * continuous traffic. Use a tick, since pdMS_TO_TICKS(1) can be
             * zero; notifications must not bypass this fairness interval. */
            vTaskDelay(1);
            yield_deadline_us = esp_timer_get_time() + IRIS_WORKER_RUN_BUDGET_US;
        } else if (runtime->link_connected && progressed) {
            (void)ulTaskNotifyTake(pdTRUE, 0);
            taskYIELD();
        } else if (ulTaskNotifyTake(pdTRUE, idle_ticks) == 0) {
            /* A timeout guarantees we blocked. An already-pending
             * notification does not, so keep the deadline in that case. */
            yield_deadline_us = esp_timer_get_time() + IRIS_WORKER_RUN_BUDGET_US;
        }
    }
#if CONFIG_ESP_IRIS_DATA_LINK
    end_session(&g_iris_data);
    iris_transport_stop(&g_iris_data);
    g_iris_data.task = NULL;
#endif
    end_session(runtime);
    iris_transport_stop(runtime);
    taskENTER_CRITICAL(&s_start_lock);
    runtime->task = NULL;
    taskEXIT_CRITICAL(&s_start_lock);
    vTaskDelete(NULL);
}

esp_err_t esp_iris_start(void)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (g_iris.started) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_OK;
    }
    if (g_iris.initializing) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_iris.initializing = true;
    if (!transition_lifecycle(&g_iris, ESP_IRIS_LIFECYCLE_STARTING)) {
        g_iris.initializing = false;
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_start_lock);

    const uint32_t services_before = iris_services_allocated_bytes();
    const uint32_t heap_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) + services_before;
    esp_err_t err = ESP_OK;
    if (!g_iris.crash_loop_initialized) {
        /* Crash-state persistence is best effort for service availability.
         * The exact failure remains visible through status/crash metadata. */
        (void)esp_iris_boot_probe();
    }
    if (!g_iris.identity_ready) {
        err = iris_identity_load_or_create(&g_iris);
        if (err == ESP_OK) {
            g_iris.identity_ready = true;
        }
    }
    if (err == ESP_OK) {
        err = iris_runtime_wire_init(&g_iris);
    }
    if (err == ESP_OK) {
        err = iris_services_init(&g_iris);
    }
    if (err == ESP_OK) {
        err = iris_transport_start(&g_iris);
    }
#if CONFIG_ESP_IRIS_DATA_LINK
    if (err == ESP_OK) {
        memcpy(g_iris_data.device_id, g_iris.device_id, sizeof(g_iris.device_id));
        memcpy(g_iris_data.hardware_mac, g_iris.hardware_mac, sizeof(g_iris.hardware_mac));
        g_iris_data.boot_id = g_iris.boot_id;
        err = iris_runtime_wire_init(&g_iris_data);
        if (err == ESP_OK) err = iris_transport_start(&g_iris_data);
    }
#endif
    if (err == ESP_OK) {
        err = iris_log_vfs_init(&g_iris);
        g_iris.vfs_registered = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = iris_log_redirect_stdio();
        g_iris.stdio_redirected = err == ESP_OK;
    }
    if (err == ESP_OK) {
        g_iris.running = true;
        g_iris.task_stack_free_min_bytes = UINT32_MAX;
        if (xTaskCreate(iris_worker, "esp_iris",
                        CONFIG_ESP_IRIS_TASK_STACK_SIZE, &g_iris,
                        CONFIG_ESP_IRIS_TASK_PRIORITY, &g_iris.task) != pdPASS) {
            g_iris.running = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        iris_services_deinit(&g_iris);
        if (g_iris.stdio_redirected) {
            (void)iris_log_restore_stdio();
            g_iris.stdio_redirected = false;
        }
        if (g_iris.vfs_registered) {
            (void)iris_log_vfs_deinit();
            g_iris.vfs_registered = false;
        }
#if CONFIG_ESP_IRIS_DATA_LINK
        iris_transport_stop(&g_iris_data);
        iris_runtime_wire_deinit(&g_iris_data);
#endif
        iris_transport_stop(&g_iris);
        iris_runtime_wire_deinit(&g_iris);
    }

    taskENTER_CRITICAL(&s_start_lock);
    g_iris.started = err == ESP_OK;
    g_iris.initializing = false;
    (void)transition_lifecycle(
        &g_iris, err == ESP_OK ? ESP_IRIS_LIFECYCLE_RUNNING
                               : ESP_IRIS_LIFECYCLE_FAILED);
    taskEXIT_CRITICAL(&s_start_lock);
    if (err == ESP_OK) {
        const uint32_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        g_iris.internal_heap_used_bytes = heap_before >= heap_after
            ? heap_before - heap_after : 0;
        g_iris.service_bytes_at_start = iris_services_allocated_bytes();
    }
    return err;
}

esp_err_t esp_iris_stop(void)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (g_iris.lifecycle == ESP_IRIS_LIFECYCLE_STOPPED) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_OK;
    }
    if (g_iris.initializing ||
            g_iris.lifecycle == ESP_IRIS_LIFECYCLE_STOPPING) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    TaskHandle_t worker = g_iris.task;
    if (worker != NULL && worker == xTaskGetCurrentTaskHandle()) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_iris.started = false;
    g_iris.running = false;
    if (!transition_lifecycle(&g_iris, ESP_IRIS_LIFECYCLE_STOPPING)) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_start_lock);

    if (worker != NULL) {
        iris_notify_worker(&g_iris);
        const TickType_t deadline = xTaskGetTickCount() +
            pdMS_TO_TICKS(IRIS_STOP_TIMEOUT_MS);
        while (g_iris.task != NULL &&
               (int32_t)(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (g_iris.task != NULL) {
            (void)transition_lifecycle(&g_iris,
                                       ESP_IRIS_LIFECYCLE_FAILED);
            return ESP_ERR_TIMEOUT;
        }
    } else {
        iris_transport_stop(&g_iris);
    }
    iris_services_deinit(&g_iris);
    const TickType_t service_deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(IRIS_STOP_TIMEOUT_MS);
    while (iris_services_work_pending() &&
           (int32_t)(service_deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (iris_services_work_pending()) {
        (void)transition_lifecycle(&g_iris, ESP_IRIS_LIFECYCLE_FAILED);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (g_iris.stdio_redirected) {
        result = iris_log_restore_stdio();
        g_iris.stdio_redirected = false;
    }
    if (g_iris.vfs_registered) {
        esp_err_t vfs_err = iris_log_vfs_deinit();
        if (result == ESP_OK) {
            result = vfs_err;
        }
        g_iris.vfs_registered = false;
    }
    taskENTER_CRITICAL(&g_iris.log_lock);
    g_iris.log_tail = 0;
    g_iris.log_used = 0;
    taskEXIT_CRITICAL(&g_iris.log_lock);
#if CONFIG_ESP_IRIS_DATA_LINK
    iris_runtime_wire_deinit(&g_iris_data);
#endif
    iris_runtime_wire_deinit(&g_iris);
    (void)transition_lifecycle(
        &g_iris, result == ESP_OK ? ESP_IRIS_LIFECYCLE_STOPPED
                                  : ESP_IRIS_LIFECYCLE_FAILED);
    return result;
}

bool esp_iris_is_started(void)
{
    return g_iris.started;
}

esp_err_t esp_iris_get_status(esp_iris_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_iris.identity_ready) {
        memset(out_status, 0, sizeof(*out_status));
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t dropped;
    taskENTER_CRITICAL(&g_iris.log_lock);
    dropped = g_iris.log_dropped_bytes;
    taskEXIT_CRITICAL(&g_iris.log_lock);
    *out_status = (esp_iris_status_t) {
        .started = g_iris.started,
        .link_connected = g_iris.link_connected,
        .session_ready = g_iris.hello_acked,
        .previous_boot_crash = g_iris.previous_boot_crash,
        .previous_boot_planned = g_iris.previous_boot_planned,
        .core_dump_present = g_iris.core_dump_present,
        .core_dump_valid = g_iris.core_dump_valid,
        .crash_loop_triggered = g_iris.crash_loop_triggered,
        .crash_recovery_pending = g_iris.crash_recovery_pending,
        .lifecycle = g_iris.lifecycle,
        .transport = iris_transport_kind(),
        .boot_id = g_iris.boot_id,
        .session_id = g_iris.session_id,
        .uptime_us = (uint64_t)esp_timer_get_time(),
        .rx_frames = g_iris.rx_frames,
        .tx_frames = g_iris.tx_frames,
        .invalid_frames = g_iris.invalid_frames,
        .link_count = g_iris.link_count,
        .log_dropped_bytes = dropped,
        .task_stack_free_min_bytes = g_iris.task_stack_free_min_bytes,
        .worker_active_max_us = g_iris.worker_active_max_us,
        .internal_heap_used_bytes = g_iris.internal_heap_used_bytes +
            (iris_services_allocated_bytes() > g_iris.service_bytes_at_start
                ? iris_services_allocated_bytes() -
                  g_iris.service_bytes_at_start : 0),
        .static_internal_bytes = runtime_static_bytes(),
        .core_dump_size = g_iris.core_dump_size <= UINT32_MAX
            ? (uint32_t)g_iris.core_dump_size : UINT32_MAX,
        .reset_reason = (uint32_t)esp_reset_reason(),
        .crash_count = g_iris.crash_count,
        .crash_limit = g_iris.crash_limit,
        .crash_origin_reset_reason = g_iris.crash_origin_reset_reason,
        .crash_failed_app_address = g_iris.crash_failed_app_address,
        .crash_failed_boot_id = g_iris.crash_failed_boot_id,
        .crash_state_error = g_iris.crash_state_error,
    };
    memcpy(out_status->device_id, g_iris.device_id,
           sizeof(out_status->device_id));
    memcpy(out_status->hardware_mac, g_iris.hardware_mac,
           sizeof(out_status->hardware_mac));
    memcpy(out_status->crash_failed_firmware_sha256,
           g_iris.crash_failed_firmware_sha256,
           sizeof(out_status->crash_failed_firmware_sha256));
    return ESP_OK;
}

esp_err_t esp_iris_boot_probe(void)
{
    if (!g_iris.identity_ready) {
        esp_err_t err = iris_identity_load_or_create(&g_iris);
        if (err != ESP_OK) {
            return err;
        }
        g_iris.identity_ready = true;
    }
    iris_crash_context_prepare(&g_iris);
    if (!g_iris.crash_initialized) {
        iris_crash_probe(&g_iris);
    }
    return iris_crash_recovery_probe(&g_iris);
}

esp_err_t esp_iris_crash_loop_reset(void)
{
    if (!g_iris.crash_loop_initialized) {
        (void)esp_iris_boot_probe();
    }
    return iris_crash_recovery_reset(&g_iris);
}

esp_err_t esp_iris_mark_planned_restart(void)
{
    if (!g_iris.started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_iris_platform_mark_planned_restart();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }
    err = iris_crash_recovery_mark_planned(&g_iris);
    if (err == ESP_OK && g_iris.hello_acked) {
        schedule_event(&g_iris, ESP_IRIS_EVENT_PLANNED_RESTART);
#if CONFIG_ESP_IRIS_DATA_LINK
        if (g_iris_data.hello_acked) schedule_event(&g_iris_data, ESP_IRIS_EVENT_PLANNED_RESTART);
#endif
    }
    return err;
}

esp_err_t esp_iris_mark_healthy(void)
{
    if (!g_iris.started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_iris_platform_mark_healthy();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }
    g_iris.healthy = true;
    if (g_iris.hello_acked) {
        schedule_event(&g_iris, ESP_IRIS_EVENT_HEALTHY);
#if CONFIG_ESP_IRIS_DATA_LINK
        if (g_iris_data.hello_acked) schedule_event(&g_iris_data, ESP_IRIS_EVENT_HEALTHY);
#endif
    }
    return ESP_OK;
}

esp_err_t esp_iris_format_device_id(char out[33])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_iris.started && !g_iris.initializing) {
        out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(g_iris.device_id); ++i) {
        out[i * 2] = hex[g_iris.device_id[i] >> 4];
        out[i * 2 + 1] = hex[g_iris.device_id[i] & 0x0f];
    }
    out[32] = '\0';
    return ESP_OK;
}
