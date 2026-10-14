// SPDX-License-Identifier: Apache-2.0
/* Test-build-only A/B control, injected into ideas_worker.c by CMake. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if CONFIG_NAND_IDEAS_DRAM_STACK || CONFIG_NAND_IDEAS_RTC_STACK
#if CONFIG_NAND_IDEAS_RTC_STACK
#define PROBE_STACK_CAP MALLOC_CAP_RTCRAM
#else
#define PROBE_STACK_CAP MALLOC_CAP_DMA
#endif
/* The production worker is persistent. Override only its stack capability in
 * this diagnostic translation unit; preserve lifetime, size and scheduling. */
#define xTaskCreatePinnedToCoreWithCaps(fn, name, size, arg, priority, handle, core, caps) \
    xTaskCreatePinnedToCoreWithCaps(fn, name, size, arg, priority, handle, core, \
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | PROBE_STACK_CAP)
#endif
