/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Recognize the held, opposing DTR/RTS phases used by both the old Mosaico
 * BSP's USBJTAGSerialReset and esptool's Classic/UnixTightReset. Ordinary
 * terminal open/close (both asserted, then released) is not a download request.
 * Only the USB task owns this state. Values are CDC assertion booleans. */
typedef struct {
    unsigned line;
    unsigned armed;
    int64_t armed_at;
} iris_usb_download_sequence_t;

static inline bool iris_usb_download_sequence(iris_usb_download_sequence_t *state,
                                              bool dtr, bool rts, int64_t now)
{
    const unsigned line = (dtr ? 1U : 0U) | (rts ? 2U : 0U);
    if (line == state->line) return false; /* Windows repeats control requests. */
    state->line = line;
    if (line == 0) {
        state->armed = 0;
    } else if (line != 3) {
        const int64_t held = now - state->armed_at;
        if (state->armed && state->armed != line && held >= 40000 && held <= 1500000) {
            state->armed = 0;
            return true;
        }
        state->armed = line;
        state->armed_at = now;
    }
    return false;
}
