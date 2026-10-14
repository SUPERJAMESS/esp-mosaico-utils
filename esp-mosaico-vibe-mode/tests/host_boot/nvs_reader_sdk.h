// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
       ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_INVALID_STATE,
       ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_PART_NOT_FOUND, ESP_FAIL };
enum { NVS_TYPE_U8 = 1, NVS_TYPE_U32 = 4,
       ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_NVS = 2 };
#define NVS_CONST_PAGE_SIZE 4096
#define NVS_CONST_PAGE_STATE_ACTIVE 0xfffffffeU
#define NVS_CONST_PAGE_STATE_FULL 0xfffffffcU
#define NVS_CONST_PAGE_STATE_FREEING 0xfffffff8U
typedef struct { size_t size; } esp_partition_t;
typedef struct { uint32_t page_state, sequence_number; } nvs_bootloader_page_header_t;
typedef struct { uint8_t bytes[32]; } nvs_bootloader_page_entry_states_t;
typedef struct {
    uint8_t namespace_index, data_type;
    char key[16];
    struct { struct { uint8_t data[8]; } primitive_type; } data;
} nvs_bootloader_single_entry_t;
typedef struct {
    esp_err_t error;
    uint32_t state, sequence;
    size_t count;
    nvs_bootloader_single_entry_t items[4];
} fake_page_t;
static fake_page_t pages[3];
static bool partition_present = true;
static int reads, fail_read;
static const esp_partition_t partition = { sizeof(pages) / sizeof(pages[0]) * NVS_CONST_PAGE_SIZE };
static const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS);
    assert(strcmp(name, "sysmeta") == 0);
    return partition_present ? &partition : NULL;
}
static esp_err_t nvs_bootloader_read_page_header(const esp_partition_t *part, size_t page,
                                                nvs_bootloader_page_header_t *header)
{
    (void)part;
    if (++reads == fail_read) return ESP_FAIL;
    header->page_state = pages[page].state;
    header->sequence_number = pages[page].sequence;
    return pages[page].error;
}
static esp_err_t nvs_bootloader_read_page_entry_states(const esp_partition_t *part, size_t page,
                                                      nvs_bootloader_page_entry_states_t *states)
{
    (void)part; (void)page;
    assert((uintptr_t)states % sizeof(uint32_t) == 0);
    return ++reads == fail_read ? ESP_FAIL : ESP_OK;
}
static esp_err_t nvs_bootloader_read_next_single_entry_item(const esp_partition_t *part, size_t page,
    const nvs_bootloader_page_entry_states_t *states, uint8_t *next, nvs_bootloader_single_entry_t *item)
{
    (void)part; (void)states;
    if (++reads == fail_read) return ESP_FAIL;
    if (*next >= pages[page].count) return ESP_ERR_NVS_NOT_FOUND;
    *item = pages[page].items[(*next)++];
    return ESP_OK;
}
