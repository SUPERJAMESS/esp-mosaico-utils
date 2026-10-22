#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define CONFIG_ESP_IRIS_LOG_RING_BYTES 2048
#define IRIS_LOG_RECORD_HEADER_SIZE 12
#define IRIS_LOG_RECORD_DATA_MAX 240
#define IRIS_LOG_DEFERRED_CONSOLE 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_VFS_FLAG_STATIC 1
#define ESP_VFS_FLAG_CONTEXT_PTR 2
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
typedef int esp_err_t;
typedef int esp_iris_transport_kind_t;
typedef int _lock_t;
typedef struct {
    bool data_link, vfs_registered, stdio_redirected;
    void *task;
    int log_lock;
    size_t log_tail, log_used;
    uint32_t log_dropped_bytes, log_dropped_reported;
} iris_runtime_t;
typedef struct {
    uint64_t monotonic_us;
    uint32_t dropped_total;
    uint8_t source, flags;
    uint16_t length;
    uint8_t data[IRIS_LOG_RECORD_DATA_MAX];
} iris_log_record_t;
struct stat;
typedef struct {
    ssize_t (*write_p)(void *, int, const void *, size_t);
    int (*open_p)(void *, const char *, int, int);
    int (*close_p)(void *, int);
    int (*fstat_p)(void *, int, struct stat *);
} esp_vfs_fs_ops_t;
extern iris_runtime_t g_iris;
static void _lock_acquire_recursive(_lock_t *lock) { }
static void _lock_release_recursive(_lock_t *lock) { }
static void xTaskNotifyGive(void *task) { }
static esp_iris_transport_kind_t iris_runtime_transport_kind(const iris_runtime_t *r) { return 0; }
static int64_t esp_timer_get_time(void) { return 1000; }
static uint16_t iris_get_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void iris_put_le16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static uint64_t iris_get_le64(const uint8_t *p) {
    uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n |= (uint64_t)p[i] << (i * 8); return n;
}
static void iris_put_le64(uint8_t *p, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) p[i] = n >> (i * 8);
}
esp_err_t iris_log_redirect_stdio(void);
esp_err_t iris_log_restore_stdio(void);
esp_err_t esp_vfs_register_fs(const char *, const esp_vfs_fs_ops_t *, int, void *);
esp_err_t esp_vfs_unregister_fs(const char *);
FILE *host_vfs_open(const char *, const char *);
