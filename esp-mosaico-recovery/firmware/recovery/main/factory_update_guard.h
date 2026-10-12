// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
/* Also stays true after a layout commit, until IDF reloads partitions at boot. */
bool factory_system_update_writer_busy(void);
