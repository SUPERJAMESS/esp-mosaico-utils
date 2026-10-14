// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#define MOSAICO_BOOT_LCD_HANDOFF_REG   LP_SYSTEM_REG_LP_STORE15_REG
#define MOSAICO_BOOT_LCD_HANDOFF_MAGIC UINT32_C(0x4D4C4344) /* "MLCD" */

static inline void mosaico_boot_handoff_clear(void)
{
    REG_WRITE(MOSAICO_BOOT_LCD_HANDOFF_REG, 0);
}

static inline void mosaico_boot_handoff_publish(void)
{
    REG_WRITE(MOSAICO_BOOT_LCD_HANDOFF_REG, MOSAICO_BOOT_LCD_HANDOFF_MAGIC);
}
