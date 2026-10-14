// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#include "factory_system_update.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"

esp_err_t update_prepare_update(const esp_iris_system_update_manifest_t *manifest, void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    return update_prepare_update_owned(ctx, manifest, NULL, FACTORY_SYSTEM_UPDATE_OWNER_ESP_IRIS,
                                       false);
}

void update_abort_update(const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
                         esp_err_t reason, void *user_ctx)
{
    update_context_t *ctx = user_ctx;

    update_abort_update_owned(ctx, operation_id, reason, FACTORY_SYSTEM_UPDATE_OWNER_ESP_IRIS);
}

#endif
