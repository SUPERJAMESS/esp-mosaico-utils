// SPDX-License-Identifier: Apache-2.0
#include "factory_system_update.h"
#include "sdkconfig.h"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE && CONFIG_IRIS_FACTORY_SYSTEM_UPDATE_BACKEND
#include "update_internal.h"
#include <stdio.h>

bool factory_system_update_recovery_supports(const char *minimum) {
    return !minimum || !*minimum ||
           factory_recovery_version_satisfies(esp_app_get_description()->version, minimum);
}
esp_err_t factory_system_update_check_idea(const char *manifest, const char *directory) {
    if (!manifest || !directory || strlen(manifest) > 3072)
        return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(manifest);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "components"));
    if (count < 2 || count > 8) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    update_context_t ctx = {.owner = FACTORY_SYSTEM_UPDATE_OWNER_NAND,
                            .lock = portMUX_INITIALIZER_UNLOCKED};
    ctx.transaction.plan = calloc(count, sizeof(*ctx.transaction.plan));
    esp_err_t err = ctx.transaction.plan ? update_parse_manifest_root(&ctx, root, count, false)
                                         : ESP_ERR_NO_MEM;
    for (int i = 0; err == ESP_OK && i < count; ++i) {
        const factory_update_plan_component_t *plan = &ctx.transaction.plan[i];
        const esp_iris_system_update_component_t *c = &plan->descriptor;
        if (c->kind != ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE &&
            c->kind != ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION &&
            c->kind != ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA) {
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (c->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA)
            continue;
        char path[FACTORY_SYSTEM_UPDATE_PATH_BYTES];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, plan->filename);
        if (n < 0 || n >= sizeof(path) || c->size > 16U * 1024U * 1024U) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        FILE *file = fopen(path, "rb");
        uint8_t *data = heap_caps_malloc(c->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!file || !data)
            err = ESP_ERR_NO_MEM;
        if (err == ESP_OK &&
            (fread(data, 1, c->size, file) != c->size || fgetc(file) != EOF || ferror(file)))
            err = ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK)
            err = c->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE
                      ? update_validate_partition_table(&ctx, data)
                      : update_validate_memory_image(data, c->size, "Idea");
        free(data);
        if (file)
            fclose(file);
    }
    free(ctx.transaction.plan);
    cJSON_Delete(root);
    return err;
}
#else
bool factory_system_update_recovery_supports(const char *minimum) {
    (void)minimum;
    return false;
}
esp_err_t factory_system_update_check_idea(const char *manifest, const char *directory) {
    (void)manifest;
    (void)directory;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
