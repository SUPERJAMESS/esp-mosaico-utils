// SPDX-License-Identifier: Apache-2.0
#include "ideas_internal.h"

bool ideas_cancelled(ideas_context_t *ctx, uint32_t request_id)
{
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    bool value = ctx->cancel_requested || request_id != ctx->state->preview_id;
    xSemaphoreGive(ctx->lock);
    return value;
}

void ideas_free_pending(pending_t *p)
{
    if (!p)
        return;
    free(p->json);
    for (size_t i = 0; i < IDEAS_MAX_FILES; ++i)
        free(p->assets[i].data);
    for (unsigned i = 0; i < 2; ++i)
        free(p->pixels[i].data);
    free(p);
}

bool ideas_ascii_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 32 || *p > 126)
            return false;
    return true;
}

int ideas_asset_index(const ideas_package_t *p, const char *name)
{
    for (size_t i = 0; i < p->file_count; ++i)
        if (!strcmp(p->files[i].name, name))
            return (int)i;
    return -1;
}

esp_err_t ideas_prepare_assets(ideas_context_t *ctx, const job_t *job, pending_t *pending,
                               bool local)
{
    pending->owner = ctx;
    const ideas_package_t *p = &pending->package;
    if (!p->presentation[0] && (!ideas_ascii_text(p->name) || !ideas_ascii_text(p->description)))
        return ESP_ERR_NOT_SUPPORTED;
    const char *names[4] = {p->presentation, p->icon, p->fonts[0], p->fonts[1]};
    for (unsigned i = 0; i < 4; ++i)
        if (*names[i]) {
            int index = ideas_asset_index(p, names[i]);
            if (index < 0)
                return ESP_ERR_INVALID_ARG;
            esp_err_t err = ideas_fetch_asset(ctx, job, pending, index, local);
            if (err != ESP_OK)
                return err;
        }
    if (p->icon[0]) {
        const icon_t *icon = &pending->assets[ideas_asset_index(p, p->icon)];
        const unsigned char *h = icon->data;
        if (icon->size < 24 || memcmp(h, "\x89PNG\r\n\x1a\n", 8) || memcmp(h + 12, "IHDR", 4) ||
            h[16] || h[17] || h[18] || !h[19] || h[19] > 96 || h[20] || h[21] || h[22] || !h[23] ||
            h[23] > 96)
            return ESP_ERR_INVALID_ARG;
    }
    if (!p->presentation[0])
        return ESP_OK;
    ideas_display_sources_t sources = {.name = p->name,
                                       .description = p->description,
                                       .locale = p->locale,
                                       .icon = p->icon,
                                       .font_count = p->font_count};
    for (size_t i = 0; i < p->font_count; ++i) {
        const icon_t *asset = &pending->assets[ideas_asset_index(p, p->fonts[i])];
        sources.fonts[i] = (ideas_font_source_t){p->fonts[i], asset->data, asset->size};
    }
    ideas_presentation_t *display = malloc(sizeof(*display));
    if (!display)
        return ESP_ERR_NO_MEM;
    const icon_t *json = &pending->assets[ideas_asset_index(p, p->presentation)];
    esp_err_t err = ideas_presentation_parse(json->data, json->size, &sources, display)
                        ? ESP_OK
                        : ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; err == ESP_OK && i < 2; ++i) {
        size_t count = i ? IDEAS_DESCRIPTION_WIDTH * IDEAS_DESCRIPTION_HEIGHT
                         : IDEAS_TITLE_WIDTH * IDEAS_TITLE_HEIGHT;
        pending->pixels[i].data = heap_caps_malloc(count * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pending->pixels[i].size = count * 2;
        if (!pending->pixels[i].data)
            err = ESP_ERR_NO_MEM;
        else if (!ideas_presentation_render(display, i, i ? 0xbdf8 : 0xffff, 0,
                                            pending->pixels[i].data, count))
            err = ESP_ERR_INVALID_ARG;
    }
    free(display);
    return err;
}

void ideas_preview_item(factory_idea_t *item, const ideas_package_t *p)
{
    memset(item, 0, sizeof(*item));
    strcpy(item->code, p->code);
    strcpy(item->application_id, p->application_id);
    strcpy(item->release_id, p->release_id);
    strcpy(item->version, p->version);
    strcpy(item->name, p->name);
    strcpy(item->description, p->description);
    item->bytes = p->total_size;
    item->components = p->component_count;
}

esp_err_t ideas_prepare_download(ideas_context_t *ctx, job_t *job)
{
    pending_t *pending = calloc(1, sizeof(*pending));
    ideas_package_t *local = calloc(1, sizeof(*local));
    if (!pending || !local) {
        free(pending);
        free(local);
        return ESP_ERR_NO_MEM;
    }
    job->pending = pending;
    /* Only reading the local descriptor requires a NAND lease. Network IO,
     * the preview and the wait for user confirmation never retain it. */
    esp_err_t err = app_storage_acquire(), existing = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        existing = ideas_store_read(ROOT, job->code, local);
        app_storage_release();
    }
    if (err == ESP_OK && job->command == IDEAS_CHECK && existing != ESP_OK)
        err = existing;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    bool full = ctx->state->count >= IDEAS_MAX_ITEMS;
    xSemaphoreGive(ctx->lock);
    if (err == ESP_OK && full && existing != ESP_OK)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && ideas_cancelled(ctx, job->request_id))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK)
        err = ideas_resolve(ctx, job, existing == ESP_OK ? local : NULL, &pending->json,
                            &pending->json_size);
    ideas_package_t *remote = &pending->package;
    if (err == ESP_OK)
        err = ideas_package_parse(pending->json, pending->json_size, job->code, remote);
    uint32_t flash_size = 0;
    esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (err == ESP_OK &&
        (remote->flash_size != flash_size ||
         (existing == ESP_OK && strcmp(local->application_id, remote->application_id))))
        err = ESP_ERR_INVALID_ARG;
    if (job->metadata_only) {
        job->up_to_date = err == ESP_OK && existing == ESP_OK && !strcmp(local->release_id, remote->release_id);
        free(local);
        return err;
    }
    if (err == ESP_OK && existing == ESP_OK && !strcmp(local->release_id, remote->release_id)) {
        err = app_storage_acquire();
        if (err == ESP_OK) {
            pending->already_downloaded =
                ideas_store_verify(ROOT, remote, ideas_verify_file, NULL) == ESP_OK;
            app_storage_release();
        }
    }
    /* Local verification may outlive Cancel; do not start another HTTP request. */
    if (err == ESP_OK && ideas_cancelled(ctx, job->request_id))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && job->command == IDEAS_CHECK && pending->already_downloaded) {
        job->up_to_date = true;
        ideas_message(ctx, "Already up to date", "The downloaded Idea is up to date.");
    } else if (err == ESP_OK) {
        err = ideas_prepare_assets(ctx, job, pending, false);
        if (err == ESP_OK && ideas_cancelled(ctx, job->request_id))
            err = ESP_ERR_INVALID_STATE;
    }
    free(local);
    return err;
}
