// SPDX-License-Identifier: Apache-2.0
#pragma once

// Keep the managed adapter's complete configuration. Only the uninitialized
// device endpoint buffers change section; no managed source is patched.
#include "tusb_config.h"

#if CONFIG_TINYUSB_MODE_DMA
// IDF maps .bss.* to internal .dram0.bss and clears it before constructors and
// USB startup. DRAM_ATTR instead puts zero-filled DMA buffers in the image.
// Preserve the adapter's alignment, including cached internal SRAM targets.
#undef CFG_TUD_MEM_SECTION
#if CONFIG_CACHE_L1_CACHE_LINE_SIZE && CONFIG_SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#define CFG_TUD_MEM_SECTION \
    __attribute__((section(".bss.vibe_usb_dma"), aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))
#else
#define CFG_TUD_MEM_SECTION \
    __attribute__((section(".bss.vibe_usb_dma"), aligned(4)))
#endif
#endif
