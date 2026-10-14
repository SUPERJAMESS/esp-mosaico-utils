// SPDX-License-Identifier: Apache-2.0
/* Read-only diagnostic: identical operations with default, HP SRAM and LP RAM
 * stacks. Explicitly injected into a hardware test build; never a product RPC. */
#include "app_storage.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>

static TaskHandle_t parent;
static unsigned mode, repeat;
static void sample(void *arg) {
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    esp_err_t err = buffer ? app_storage_acquire() : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        FILE *evict = fopen(CONFIG_NAND_READ_PROBE_EVICT_PATH, "rb");
        if (evict) { while (fread(buffer, 1, 4096, evict)) {} fclose(evict); }
        else err = ESP_FAIL;
        app_storage_release();
        if (err == ESP_OK) err = app_storage_acquire();
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t digest[32]; size_t digest_size = 0, bytes = 0;
    int64_t read_us = 0, hash_us = 0;
    if (err == ESP_OK) {
        FILE *f = fopen(CONFIG_NAND_READ_PROBE_PATH, "rb");
        if (!f || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) err = ESP_FAIL;
        while (err == ESP_OK) {
            int64_t t = esp_timer_get_time();
            size_t n = fread(buffer, 1, 4096, f);
            read_us += esp_timer_get_time() - t;
            if (!n) break;
            t = esp_timer_get_time();
            if (psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) err = ESP_FAIL;
            hash_us += esp_timer_get_time() - t;
            bytes += n;
            vTaskDelay(1); /* Match the production hashing loop. */
        }
        if (f) { if (ferror(f)) err = ESP_FAIL; fclose(f); }
        if (err == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS) err = ESP_FAIL;
        app_storage_release();
    }
    psa_hash_abort(&hash);
    char hex[65] = {0};
    if (digest_size == 32) for (unsigned i = 0; i < 32; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    ESP_LOGI("runtime_probe", "nand mode=%u repeat=%u core=%u stack=%p hash=%p buffer=%p bytes=%u read_us=%lld hash_us=%lld sha256=%s result=%d",
             mode, repeat, (unsigned)xPortGetCoreID(), (void *)&err, (void *)&hash, buffer,
             (unsigned)bytes, (long long)read_us, (long long)hash_us, hex, err);
    uint8_t local[1024];
    for (unsigned kind = 0; kind < 2 && err == ESP_OK; ++kind) {
        uint8_t *data = kind ? buffer : local;
        read_us = hash_us = 0;
        if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) err = ESP_FAIL;
        for (uint32_t done = 0; done < bytes && err == ESP_OK;) {
            size_t n = bytes - done < 1024 ? bytes - done : 1024;
            int64_t t = esp_timer_get_time();
            err = esp_flash_read(esp_flash_default_chip, data, 0x210000 + done, n);
            read_us += esp_timer_get_time() - t;
            t = esp_timer_get_time();
            if (err == ESP_OK && psa_hash_update(&hash, data, n) != PSA_SUCCESS) err = ESP_FAIL;
            hash_us += esp_timer_get_time() - t;
            done += n;
        }
        if (err == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS) err = ESP_FAIL;
        psa_hash_abort(&hash);
        if (digest_size == 32) for (unsigned i = 0; i < 32; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
        ESP_LOGI("runtime_probe", "nor mode=%u repeat=%u core=%u buffer=%s bytes=%u read_us=%lld hash_us=%lld sha256=%s result=%d",
                 mode, repeat, (unsigned)xPortGetCoreID(), kind ? "heap" : "stack", (unsigned)bytes,
                 (long long)read_us, (long long)hash_us, hex, err);
    }
    free(buffer);
    xTaskNotifyGive(parent);
    vTaskSuspend(NULL); /* Parent deletes WithCaps tasks and releases both allocations. */
}
void runtime_probe_sequence(void *arg) {
    parent = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(60000));
    for (repeat = 0; repeat < 2; ++repeat) for (mode = 0; mode < 4; ++mode) {
        unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if (mode == 2) caps |= MALLOC_CAP_DMA;
        if (mode == 3) caps |= MALLOC_CAP_RTCRAM;
        TaskHandle_t task;
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(sample, "runtime_sample",
            mode == 0 ? 8192 : 16384, NULL, 3, &task, repeat, caps);
        if (ok == pdPASS) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            vTaskDeleteWithCaps(task);
        } else ESP_LOGW("runtime_probe", "mode=%u allocation failed caps=%u", mode, caps);
    }
    ESP_LOGI("runtime_probe", "complete; no files or flash written");
    vTaskDelete(NULL);
}
