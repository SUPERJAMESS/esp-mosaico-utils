#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_BRIDGE_ORIGIN_BYTES 256
#define IRIS_BRIDGE_BOARD_BYTES 64
#define IRIS_BRIDGE_CODE_BYTES 16

typedef struct {
    const char *server_url;
    const char *board_id;
    const char *device_id;
    bool enable_factory_update;    /* Recovery self-update; default false. */
    bool enable_system_update;     /* .irisfw transactions; default false. */
    bool enable_bootloader_update; /* Requires system_update; default false. */
    /* Register/cache a code without polling for or executing remote work. */
    bool prefetch_only;
    /* Nonblocking readiness probe; called only by the Bridge worker. */
    bool (*network_ready)(void);
} iris_bridge_config_t;

typedef struct {
    bool running;
    char state[24];
    char server_url[IRIS_BRIDGE_ORIGIN_BYTES];
    char code[IRIS_BRIDGE_CODE_BYTES];
    /* RFC 3339 with nanoseconds and a numeric timezone needs 35 characters. */
    char expires_at[40];
    uint32_t expires_in_ms;
    unsigned progress;
    esp_err_t error;
} iris_bridge_snapshot_t;

/* Validate persisted Bridge intent and restore the next boot selection.
 * Call after sysmeta initialization, before UI/network; returns storage/policy errors. */
esp_err_t iris_bridge_resume_boot(void);
/* Copy required config and its strings, then start one worker waiting for IP.
 * network_ready must remain callable until stopped. Task context; returns
 * configuration/admission/allocation errors without waiting for the network. */
esp_err_t iris_bridge_start(const iris_bridge_config_t *config);
/* Request asynchronous stop; the worker owns clients until is_running is false.
 * Nonblocking and task-safe; an authorized critical commit still finishes. */
void iris_bridge_stop(void);
/* Opening the download page enables polling; leaving it pauses polling.
 * An already-authorized critical commit still finishes. */
void iris_bridge_set_active(bool active);
/* Return whether the Bridge worker or its shutdown sequence is active.
 * Atomic/nonblocking; does not create a session. */
bool iris_bridge_is_running(void);
/* Copy current session telemetry into snapshot; NULL is ignored.
 * Task-safe, nonblocking; expired pairing codes are cleared in the copied view. */
void iris_bridge_get_snapshot(iris_bridge_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
