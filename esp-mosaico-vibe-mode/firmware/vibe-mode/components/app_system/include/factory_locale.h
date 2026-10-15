// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
/* 0: zh-CN; 1: en (default). Persisted in sysmeta/vibe_prefs/lang as 2/1. */
void factory_locale_init(void);
unsigned factory_locale_get(void);
/* Apply the product settings snapshot without a second persistent write. */
void factory_locale_use(unsigned language);
esp_err_t factory_locale_set(unsigned language);
