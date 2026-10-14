// SPDX-License-Identifier: Apache-2.0
#include "app_storage.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"

static esp_gsp_handle_t s_ui;
static atomic_bool s_load, s_stop;
static TaskHandle_t s_parent;
static unsigned s_mode, s_repeat;
extern esp_err_t iris_screen_mirror_attach(esp_gsp_handle_t ui);

static void load_ui(void *arg) {
    unsigned n = 0;
    while (!atomic_load(&s_stop)) {
        if (atomic_load(&s_load)) {
            char text[64];
            snprintf(text, sizeof(text), "NAND read test %u", ++n);
            (void)esp_gsp_set_text(s_ui, GSP_VIBE_BIND_ADDRESS, text);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}
static void read_probe(void *arg) {
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = buffer ? app_storage_acquire() : ESP_ERR_NO_MEM;
    if (err == ESP_OK && CONFIG_IRIS_FACTORY_NAND_READ_CACHE_KIB > 0) {
        FILE *evict = fopen(CONFIG_NAND_READ_PROBE_EVICT_PATH, "rb");
        if (evict) {
            while (fread(buffer, 1, 4096, evict)) {}
            if (ferror(evict)) err = ESP_FAIL;
            fclose(evict);
        } else err = ESP_FAIL;
        app_storage_release();
        if (err == ESP_OK) err = app_storage_acquire();
    }
    int64_t started = esp_timer_get_time(), read_us = 0, hash_us = 0;
    size_t bytes = 0, hash_size = 0;
    unsigned char hash[32] = {0};
    psa_hash_operation_t state = PSA_HASH_OPERATION_INIT;
    if (err == ESP_OK) {
        FILE *f = fopen(CONFIG_NAND_READ_PROBE_PATH, "rb");
        if (!f || psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&state, PSA_ALG_SHA_256) != PSA_SUCCESS)
            err = ESP_FAIL;
        while (err == ESP_OK) {
            int64_t t = esp_timer_get_time();
            size_t n = fread(buffer, 1, 4096, f);
            read_us += esp_timer_get_time() - t;
            if (!n) break;
            bytes += n;
            t = esp_timer_get_time();
            if (psa_hash_update(&state, buffer, n) != PSA_SUCCESS) err = ESP_FAIL;
            hash_us += esp_timer_get_time() - t;
        }
        if (f && ferror(f)) err = ESP_FAIL;
        if (f) fclose(f);
        if (err == ESP_OK && (psa_hash_finish(&state, hash, sizeof(hash), &hash_size) != PSA_SUCCESS || hash_size != 32)) err = ESP_FAIL;
        app_storage_release();
    }
    psa_hash_abort(&state);
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i*2, 3, "%02x", hash[i]);
    ESP_LOGI("nand_probe", "mode=%u repeat=%u core=%u bytes=%u total_us=%lld read_us=%lld hash_us=%lld sha256=%s result=%d",
             s_mode, s_repeat, (unsigned)xPortGetCoreID(), (unsigned)bytes,
             (long long)(esp_timer_get_time()-started), (long long)read_us, (long long)hash_us, hex, err);
    free(buffer);
    xTaskNotifyGive(s_parent);
    vTaskDelete(NULL);
}
static void sequence(void *arg) {
    s_parent = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(60000));
    TaskHandle_t load_task;
    if (xTaskCreate(load_ui, "nand_ui_load", 4096, NULL, 2, &load_task) != pdPASS) { vTaskDelete(NULL); return; }
    for (s_repeat = 0; s_repeat < 2; ++s_repeat) {
        for (s_mode = 0; s_mode < 3; ++s_mode) {
            esp_gsp_esp_lcd_pause_t *pause = NULL;
            if (s_mode == 0 && esp_gsp_esp_lcd_pause(s_ui, 3000, &pause) != ESP_OK) continue;
            atomic_store(&s_load, s_mode == 2);
            for (unsigned core = 0; core < 2; ++core) {
                if (xTaskCreatePinnedToCore(read_probe, "nand_read_probe", 8192, NULL, 3, NULL, core) == pdPASS)
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            atomic_store(&s_load, false);
            if (pause && esp_gsp_esp_lcd_resume_paused(pause, &s_ui) != ESP_OK) {
                ESP_LOGE("nand_probe", "UI resume failed");
                atomic_store(&s_stop, true);
                vTaskDelete(NULL);
                return;
            }
        }
    }
    atomic_store(&s_stop, true);
    ESP_LOGI("nand_probe", "complete; UI resumed; no files written");
    vTaskDelete(NULL);
}
extern void runtime_probe_sequence(void *arg);
#ifndef CONFIG_NAND_RUNTIME_PROBE
#define CONFIG_NAND_RUNTIME_PROBE 0
#endif
esp_err_t nand_probe_attach(esp_gsp_handle_t ui) {
    esp_err_t err = iris_screen_mirror_attach(ui);
    if (err == ESP_OK && CONFIG_NAND_READ_PROBE_PATH[0]) {
        s_ui = ui;
        if (xTaskCreate(CONFIG_NAND_RUNTIME_PROBE ? runtime_probe_sequence : sequence, "nand_probe_seq", 4096, NULL, 2, NULL) != pdPASS)
            return ESP_ERR_NO_MEM;
    }
    return err;
}
