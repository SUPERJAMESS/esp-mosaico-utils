#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
extern int fail_allocation;
static inline void *heap_caps_calloc(size_t n,size_t z,unsigned caps){if(fail_allocation && --fail_allocation==0)return NULL;return calloc(n,z);}
static inline void *heap_caps_malloc(size_t z,unsigned caps){if(fail_allocation && --fail_allocation==0)return NULL;return malloc(z);}
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t p=malloc(sizeof(*p));assert(p);pthread_mutex_init(p,NULL);return p;}
static inline void xSemaphoreTake(SemaphoreHandle_t p,int timeout){pthread_mutex_lock(p);}
static inline void xSemaphoreGive(SemaphoreHandle_t p){pthread_mutex_unlock(p);}
static inline void vSemaphoreDelete(SemaphoreHandle_t p){pthread_mutex_destroy(p);free(p);}
static inline int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000LL+t.tv_nsec/1000;}
typedef void *spi_device_handle_t;
typedef struct esp_blockdev *esp_blockdev_handle_t;
typedef struct {
 esp_err_t (*read)(esp_blockdev_handle_t,uint8_t*,size_t,uint64_t,size_t);
 esp_err_t (*write)(esp_blockdev_handle_t,const uint8_t*,uint64_t,size_t);
 esp_err_t (*erase)(esp_blockdev_handle_t,uint64_t,size_t);
 esp_err_t (*sync)(esp_blockdev_handle_t);
 esp_err_t (*ioctl)(esp_blockdev_handle_t,uint8_t,void*);
 esp_err_t (*release)(esp_blockdev_handle_t);
} esp_blockdev_ops_t;
typedef struct {uint64_t disk_size;size_t read_size,write_size,erase_size;} esp_blockdev_geometry_t;
typedef struct esp_blockdev {void *ctx;unsigned device_flags;esp_blockdev_geometry_t geometry;const esp_blockdev_ops_t *ops;} esp_blockdev_t;
typedef struct {uint16_t cmd;uint64_t addr;size_t length,rxlength;unsigned flags;const void *tx_buffer;void *rx_buffer;uint8_t rx_data[4];} spi_transaction_t;
#define SPI_TRANS_USE_RXDATA 1
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
typedef pthread_mutex_t portMUX_TYPE;
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
esp_err_t spi_device_polling_transmit(spi_device_handle_t,spi_transaction_t*);
