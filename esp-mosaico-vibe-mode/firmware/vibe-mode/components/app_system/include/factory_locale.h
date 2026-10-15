// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
/* 0: zh-CN (first boot); 1: en. The setting is independent of Idea releases. */
void factory_locale_init(void);
unsigned factory_locale_get(void);
/* Apply the product settings snapshot without a second persistent write. */
void factory_locale_use(unsigned language);
esp_err_t factory_locale_set(unsigned language);
