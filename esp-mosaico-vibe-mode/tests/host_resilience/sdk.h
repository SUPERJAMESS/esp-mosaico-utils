#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_WIFI_BASE 0x3000
#define ESP_ERR_WIFI_NOT_CONNECT (ESP_ERR_WIFI_BASE + 15)
#define ESP_ERROR_CHECK(e) assert((e) == ESP_OK)
#define ESP_RETURN_ON_ERROR(e, tag, ...) do { (void)(tag); int code = (e); if (code) return code; } while (0)
#define ESP_RETURN_ON_FALSE(c, e, tag, ...) do { (void)(tag); if (!(c)) return (e); } while (0)
#define ESP_GOTO_ON_ERROR(e, label, tag, ...) do { (void)(tag); ret = (e); if (ret) goto label; } while (0)
static inline void mock_log(const char *format, ...) { }
#define ESP_LOGI(tag, ...) ((void)(tag), mock_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), mock_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), mock_log(__VA_ARGS__))
const char *esp_err_to_name(esp_err_t err);
size_t strlcpy(char *dst, const char *src, size_t size);

typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10))
#define portMAX_DELAY UINT32_MAX
typedef struct mock_queue *QueueHandle_t;
typedef QueueHandle_t SemaphoreHandle_t;
extern int64_t mock_now;
extern void (*mock_wait)(void);
extern int mock_alloc_fail_after;
extern unsigned mock_allocations;
int64_t esp_timer_get_time(void);
QueueHandle_t xQueueCreate(unsigned count, size_t size);
int xQueueOverwrite(QueueHandle_t queue, const void *value);
int xQueueReceive(QueueHandle_t queue, void *value, TickType_t wait);
void vQueueDelete(QueueHandle_t queue);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#include "vui.h"


#define CONFIG_IRIS_FACTORY_MDNS_PREFIX "mosaico"
#define CONFIG_ESP_IRIS_TCP_PORT 7777
#define CONFIG_IRIS_FACTORY_WIFI_MAX_NETWORKS 16
#define CONFIG_IRIS_FACTORY_WIFI_CONNECT_TIMEOUT_MS 15000
#include "esp_iris_system_inventory.h"
typedef struct cJSON cJSON;
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_SCAN_DONE 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_EVENT_STA_STOP 4
#define WIFI_EVENT_STA_START 5
#define IP_EVENT_STA_LOST_IP 6
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define WIFI_AUTH_OPEN 0
#define WIFI_IF_STA 0
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_PS_NONE 0
#define ESP_MAC_WIFI_STA 1
typedef int esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*mock_event_callback_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct { int dummy; } esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {
    struct {
        uint8_t ssid[32], password[64];
        int scan_method, sort_method;
        struct { int authmode; } threshold;
        struct { bool capable, required; } pmf_cfg;
    } sta;
} wifi_config_t;
typedef struct { uint8_t ssid[33]; int8_t rssi; int authmode; } wifi_ap_record_t;
typedef struct { int reason; } wifi_event_sta_disconnected_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { const char *key, *value; } mdns_txt_item_t;
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(esp_netif_t *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_ps(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_wifi_scan_start(const void *, bool);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t,
    mock_event_callback_t, void *, esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t,
    esp_event_handler_instance_t);
void esp_ip4addr_ntoa(const esp_ip4_addr_t *, char *, size_t);
esp_err_t esp_read_mac(uint8_t *, int);
esp_err_t esp_iris_format_device_id(char *);
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *);
esp_err_t mdns_instance_name_set(const char *);
esp_err_t mdns_service_add(const char *, const char *, const char *, unsigned,
    const mdns_txt_item_t *, size_t);
void mdns_free(void);

typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);

typedef struct { uint32_t address; int subtype; const char *label; } esp_partition_t;
typedef void *esp_partition_iterator_t;
typedef int esp_ota_img_states_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_SUBTYPE_APP_TEST 32
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_MAX 32
#define ESP_PARTITION_SUBTYPE_ANY 255
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_partition_iterator_t esp_partition_find(int, int, const char *);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
typedef struct { char project_name[32], version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
typedef struct { const uint8_t *payload; size_t payload_size; } esp_iris_rpc_request_t;
typedef struct { uint32_t crash_count; bool crash_recovery_pending; } esp_iris_status_t;
typedef esp_err_t (*mock_rpc_t)(const esp_iris_rpc_request_t *, uint8_t *, size_t, size_t *, void *);
esp_err_t esp_iris_rpc_register(unsigned, unsigned, mock_rpc_t, void *);
esp_err_t esp_iris_get_status(esp_iris_status_t *);
esp_err_t esp_iris_start(void);
esp_err_t esp_iris_mark_planned_restart(void);
void esp_restart(void);
#define esp_rom_printf(...) mock_log(__VA_ARGS__)
typedef struct mock_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; void *arg; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
bool esp_timer_is_active(esp_timer_handle_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
