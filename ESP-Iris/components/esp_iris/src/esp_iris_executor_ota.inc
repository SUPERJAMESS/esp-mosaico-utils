/* OTA progress snapshots; included by executor.inc with its private executor state. */
static bool executor_ota_status(esp_iris_ota_status_t *out)
{
#if CONFIG_ESP_IRIS_OTA
    if (executor_busy()) {
        taskENTER_CRITICAL(&s_services_lock);
        *out = s_executor.ota_status;
        /* BEGIN erases flash before ota->active is published. Report the
         * admitted worker as active too, so platform readers/reset cannot
         * enter between target selection and the first erase. */
        const int stage = atomic_load(&s_executor.stage);
        if ((stage == IRIS_EXEC_QUEUED || stage == IRIS_EXEC_RUNNING) &&
            s_executor.header.channel == ESP_IRIS_CHANNEL_OTA &&
            s_executor.header.type == ESP_IRIS_OTA_BEGIN)
            out->active = true;
        taskEXIT_CRITICAL(&s_services_lock);
        return true;
    }
#else
    (void)out;
#endif
    return false;
}

static void executor_snapshot(void)
{
#if CONFIG_ESP_IRIS_OTA
    uint8_t payload[25] = {0};
    esp_iris_ota_status_t status = {0};
    iris_service_state_t *state = service_state(false);
    size_t label_size = 0;
    if (valid_state(state) && state->ota != NULL) {
        iris_ota_t *ota = state->ota;
        esp_iris_job_info_t info = {0};
        if (ota->job != NULL) {
            (void)esp_iris_job_get_info(ota->job, &info);
        }
        status = (esp_iris_ota_status_t) {
            .job_id = info.id, .total_size = ota->total_size,
            .received_size = ota->received, .progress_permille = info.progress_permille,
            .state = info.state, .result = info.result, .active = ota->active,
        };
        iris_put_le32(payload, info.id);
        iris_put_le32(payload + 4, ota->total_size);
        iris_put_le32(payload + 8, ota->received);
        iris_put_le16(payload + 12, info.progress_permille);
        payload[14] = ota->active ? 1 : 0;
        label_size = ota->partition == NULL ? 0 : strnlen(ota->partition->label, 5);
        payload[15] = (uint8_t)label_size;
        iris_put_le32(payload + 16, (uint32_t)info.result);
        if (label_size != 0) memcpy(payload + 20, ota->partition->label, label_size);
    }
    taskENTER_CRITICAL(&s_services_lock);
    s_executor.ota_status = status;
    memcpy(s_executor.ota_snapshot, payload, sizeof(payload));
    s_executor.ota_snapshot_size = 20 + label_size;
    taskEXIT_CRITICAL(&s_services_lock);
#endif
}
