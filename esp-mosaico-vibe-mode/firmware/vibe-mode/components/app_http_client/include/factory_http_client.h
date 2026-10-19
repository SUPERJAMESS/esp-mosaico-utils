// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACTORY_HTTP_CLIENT_DEVICE_ID_HEADER "X-Device-ID"

/* Apply product-wide headers before every open/perform, including reused
 * clients. Identity uses the public Iris hardware ID, independently of API
 * query/body fields. If unavailable, omit it and remove any stale value.
 * Returns header-operation errors; does not open or take ownership of client. */
esp_err_t factory_http_client_set_common_headers(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
