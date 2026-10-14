// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

esp_err_t bridge_response_header(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (response && event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value && !strcasecmp(event->header_key, "Retry-After")) {
        char *end = NULL;
        unsigned long seconds = strtoul(event->header_value, &end, 10);
        if (end != event->header_value && !*end && seconds <= 3600)
            response->retry_after_seconds = (unsigned)seconds;
    }
    return ESP_OK;
}

esp_http_client_handle_t bridge_open_request_with_response(bridge_context_t *ctx, const char *path,
                                                           const char *body,
                                                           http_response_t *response,
                                                           esp_http_client_handle_t h,
                                                           int timeout_ms)
{
    memset(response, 0, sizeof(*response));
    char url[768], auth[180];
    if (path[0] != '/' ||
        snprintf(url, sizeof(url), "%s%s", ctx->cfg.server_url, path) >= (int)sizeof(url))
        goto fail;
    esp_http_client_config_t c = {.url = url,
                                  .event_handler = bridge_response_header,
                                  .user_data = response,
                                  .timeout_ms = timeout_ms,
                                  .disable_auto_redirect = true,
                                  .crt_bundle_attach = esp_crt_bundle_attach,
                                  .buffer_size = 4096};
    if (!h)
        h = esp_http_client_init(&c);
    if (!h)
        return NULL;
    if (esp_http_client_set_url(h, url) != ESP_OK ||
        esp_http_client_set_timeout_ms(h, timeout_ms) != ESP_OK)
        goto fail;
    if (ctx->token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", ctx->token);
        esp_http_client_set_header(h, "Authorization", auth);
    } else {
        esp_http_client_delete_header(h, "Authorization");
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_method(h, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    size_t length = body ? strlen(body) : 0;
    if (esp_http_client_open(h, length) != ESP_OK)
        goto fail;
    for (size_t done = 0; done < length;) {
        int n = esp_http_client_write(h, body + done, length - done);
        if (n <= 0)
            goto fail;
        done += n;
    }
    if (esp_http_client_fetch_headers(h) < 0)
        goto fail;
    response->status = esp_http_client_get_status_code(h);
    if (response->status < 200 || response->status >= 300)
        goto fail;
    return h;
fail:
    if (h) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
    return NULL;
}

void bridge_close_client(esp_http_client_handle_t *client)
{
    if (*client) {
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
    }
}

esp_http_client_handle_t bridge_open_request(bridge_context_t *ctx, const char *path,
                                             const char *body)
{
    ctx->download_client = bridge_open_request_with_response(
        ctx, path, body, &ctx->download_response, ctx->download_client, 15000);
    return ctx->download_client;
}

void bridge_finish_download(bridge_context_t *ctx, bool valid)
{
    if (ctx->download_client &&
        (!valid || !esp_http_client_is_complete_data_received(ctx->download_client) ||
         !esp_http_client_is_persistent_connection(ctx->download_client)))
        bridge_close_client(&ctx->download_client);
}

cJSON *bridge_request_with_response(bridge_context_t *ctx, const char *path, const cJSON *body,
                                    http_response_t *response, esp_http_client_handle_t *reusable,
                                    int timeout_ms, bool observer)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 2000;
    char *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !encoded)
        return NULL;
    esp_http_client_handle_t h =
        bridge_open_request_with_response(ctx, path, encoded, response, *reusable, timeout_ms);
    *reusable = h;
    free(encoded);
    if (!h)
        return NULL;
    char *buf = malloc(LIMIT + 1);
    size_t used = 0;
    cJSON *result = NULL;
    if (buf) {
        while (used < LIMIT && esp_timer_get_time() < deadline &&
               (!observer ||
                (!atomic_load(&ctx->cancel_poll_stop) && !atomic_load(&ctx->stop_requested)))) {
            size_t wanted = LIMIT - used;
            if (wanted > 4096)
                wanted = 4096;
            int n = esp_http_client_read(h, buf + used, wanted);
            if (n < 0)
                break;
            if (n == 0) {
                if (esp_http_client_is_complete_data_received(h)) {
                    buf[used] = 0;
                    result = cJSON_ParseWithLength(buf, used + 1);
                }
                break;
            }
            used += n;
        }
        free(buf);
    }
    if (!result || !esp_http_client_is_persistent_connection(h))
        bridge_close_client(reusable);
    return result;
}

cJSON *bridge_request(bridge_context_t *ctx, const char *path, const cJSON *body)
{
    return bridge_request_with_response(ctx, path, body, &ctx->main_response, &ctx->main_client,
                                        15000, false);
}

void bridge_path_session(bridge_context_t *ctx, char out[256], const char *suffix)
{
    snprintf(out, 256, "/api/v1/device-sessions/%s%s", ctx->session, suffix);
}
