// SPDX-License-Identifier: Apache-2.0
#include "esp_random.h"
#include "ideas_internal.h"
#include "ideas_multipart.h"
#include <time.h>

/* Requests run on the single Ideas worker. Header state never crosses a
 * request. */
static struct {
    char etag[68], retry_after[96], media[192];
    bool invalid, encoded;
    unsigned etags, media_count;
} headers;
static esp_err_t header_event(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER)
        return ESP_OK;
    if (!strcasecmp(event->header_key, "ETag")) {
        if (++headers.etags != 1 || strlen(event->header_value) >= sizeof(headers.etag))
            headers.invalid = true;
        else
            strcpy(headers.etag, event->header_value);
    } else if (!strcasecmp(event->header_key, "Content-Encoding"))
        headers.encoded |= strcasecmp(event->header_value, "identity") != 0;
    else if (!strcasecmp(event->header_key, "Content-Type")) {
        if (++headers.media_count != 1 || strlen(event->header_value) >= sizeof(headers.media))
            headers.invalid = true;
        else
            strcpy(headers.media, event->header_value);
    } else if (!strcasecmp(event->header_key, "Retry-After"))
        snprintf(headers.retry_after, sizeof(headers.retry_after), "%s", event->header_value);
    return ESP_OK;
}
static int timeout_ms(int64_t deadline, int maximum) {
    int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
    return remaining > maximum ? maximum : remaining > 0 ? (int)remaining : 1;
}
static bool leap(unsigned year) { return !(year % 4) && (year % 100 || !(year % 400)); }
/* HTTP dates are UTC regardless of the device's display time zone. Accept all
 * three HTTP date forms without changing the process-wide TZ environment. */
static uint64_t retry_after_ms(const char *value) {
    if (!*value)
        return 0;
    bool digits = true;
    for (const char *p = value; *p; ++p)
        digits &= *p >= '0' && *p <= '9';
    if (digits) {
        errno = 0;
        unsigned long long seconds = strtoull(value, NULL, 10);
        return errno || seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
    }
    int day = 0, year = 0, hour = 0, minute = 0, second = 0, end = 0;
    char month[4] = {0};
    (void)sscanf(value, "%*3s, %d %3s %d %d:%d:%d GMT%n", &day, month, &year, &hour, &minute,
                 &second, &end);
    if (!end || value[end]) {
        end = 0;
        const char *comma = strchr(value, ',');
        if (comma) {
            (void)sscanf(comma + 1, " %d-%3s-%d %d:%d:%d GMT%n", &day, month, &year, &hour, &minute,
                         &second, &end);
            if (end && !comma[1 + end] && year < 100) {
                time_t now = time(NULL);
                struct tm current;
                gmtime_r(&now, &current);
                year += (current.tm_year + 1900) / 100 * 100;
                if (year > current.tm_year + 1950)
                    year -= 100;
            }
            end = end && !comma[1 + end] ? (int)strlen(value) : 0;
        }
        if (!end)
            (void)sscanf(value, "%*3s %3s %d %d:%d:%d %d%n", month, &day, &hour, &minute, &second,
                         &year, &end);
    }
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *found = *month ? strstr(months, month) : NULL;
    if (!end || value[end] || !found || (found - months) % 3 || year < 1970 || year > 9999 ||
        day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return 0;
    unsigned m = (found - months) / 3;
    const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((unsigned)day > lengths[m] + (m == 1 && leap(year)))
        return 0;
    int64_t days = day - 1;
    for (unsigned y = 1970; y < (unsigned)year; ++y)
        days += leap(y) ? 366 : 365;
    for (unsigned i = 0; i < m; ++i)
        days += lengths[i] + (i == 1 && leap(year));
    int64_t delta = days * 86400 + hour * 3600 + minute * 60 + second - time(NULL);
    return delta > 0 ? (uint64_t)delta * 1000 : 0;
}
static bool cancelled(ideas_context_t *ctx, uint32_t request) {
    return request && ideas_cancelled(ctx, request);
}
static void progress(ideas_context_t *ctx, int64_t bytes) {
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->state->received += bytes;
    xSemaphoreGive(ctx->lock);
}
static esp_err_t fetch_http(ideas_context_t *ctx, uint32_t request, const char *path, void *memory,
                            size_t capacity, size_t *size, const char *destination, bool root,
                            bool manifest, ideas_multipart_t *preview) {
    const char *origin = CONFIG_IRIS_FACTORY_IDEAS_SERVER_URL;
    const char *host = origin + (strlen(origin) >= 8 ? 8 : 0);
    if (strncmp(origin, "https://", 8) || !*host || strpbrk(host, "/?#@\r\n"))
        return ESP_ERR_INVALID_STATE;
    char url[512];
    int n = snprintf(url, sizeof(url), "%s%s", origin, path);
    if (n < 0 || n >= sizeof(url))
        return ESP_ERR_INVALID_SIZE;
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    int64_t deadline = esp_timer_get_time() + (manifest ? 30000000LL : 600000000LL);
    uint8_t *block = destination || preview ? malloc(IO_BYTES) : NULL;
    if ((destination || preview) && !block)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        size_t used = 0;
        psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
        bool retry = false;
        FILE *file = NULL;
        if (cancelled(ctx, request)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (esp_timer_get_time() >= deadline) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        memset(&headers, 0, sizeof(headers));
        esp_http_client_config_t config = {.url = url,
                                           .timeout_ms = timeout_ms(deadline, 10000),
                                           .disable_auto_redirect = true,
                                           .crt_bundle_attach = esp_crt_bundle_attach,
                                           .event_handler = header_event,
                                           .buffer_size = IO_BYTES,
                                           .method = HTTP_METHOD_GET};
#if defined(IDEAS_PRIVATE_CA)
        extern const char ideas_ca[] asm("_binary_ideas_ca_pem_start");
        config.cert_pem = ideas_ca;
        config.crt_bundle_attach = NULL;
#endif
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (preview)
            esp_http_client_set_header(client, "Accept", "multipart/related");
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK)
            retry = true;
        if (err == ESP_OK) {
            esp_http_client_set_timeout_ms(client, timeout_ms(deadline, 60000));
            if (esp_http_client_fetch_headers(client) < 0) {
                err = ESP_ERR_TIMEOUT;
                retry = true;
            }
        }
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status != 200) {
                err = status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
                retry = status == 429 || status == 502 || status == 503 || status == 504;
            } else if (headers.invalid || headers.encoded ||
                       (root && (headers.media_count != 1 || strlen(headers.etag) != 66 ||
                                 headers.etag[0] != '"' || headers.etag[65] != '"')))
                err = ESP_ERR_INVALID_ARG;
        }
        int64_t length = esp_http_client_get_content_length(client);
        if (err == ESP_OK && length >= 0 && (root ? length > capacity : length != capacity))
            err = ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK && preview) {
            err = ideas_multipart_begin(preview, preview->pending, preview->code, preview->language,
                                        headers.media);
            if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS ||
                                  psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS))
                err = ESP_FAIL;
        }
        if (err == ESP_OK && destination) {
            file = fopen(destination, "wb");
            if (!file)
                err = ESP_FAIL;
        }
        while (err == ESP_OK) {
            if (cancelled(ctx, request)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (esp_timer_get_time() >= deadline) {
                err = ESP_ERR_TIMEOUT;
                retry = true;
                break;
            }
            esp_http_client_set_timeout_ms(client, timeout_ms(deadline, 60000));
            char extra;
            size_t room = capacity - used;
            int want = room > IO_BYTES ? IO_BYTES : room ? (int)room : 1;
            char *target = !room                      ? &extra
                           : (destination || preview) ? (char *)block
                                                      : (char *)memory + used;
            int64_t started = factory_io_now();
            int count = esp_http_client_read(client, target, want);
            metrics.read += factory_io_now() - started;
            if (count < 0) {
                err = ESP_ERR_TIMEOUT;
                retry = true;
                break;
            }
            if (!count)
                break;
            if ((size_t)count > room) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            metrics.bytes += count;
            if (file) {
                started = factory_io_now();
                size_t written = fwrite(target, 1, count, file);
                metrics.write += factory_io_now() - started;
                if (written != count) {
                    err = ESP_FAIL;
                    break;
                }
            }
            if (preview) {
                if (psa_hash_update(&hash, (uint8_t *)target, count) != PSA_SUCCESS)
                    err = ESP_FAIL;
                else
                    err = ideas_multipart_feed(preview, target, count);
            }
            used += count;
            if (file)
                progress(ctx, count);
            vTaskDelay(1);
        }
        if (err == ESP_OK && (!esp_http_client_is_complete_data_received(client) ||
                              (length >= 0 && used != length) || (!root && used != capacity))) {
            err = ESP_ERR_TIMEOUT;
            retry = true; /* incomplete network transfer */
        }
        if (file) {
            int64_t started = factory_io_now();
            if (fflush(file) || fsync(fileno(file))) {
                err = ESP_FAIL;
                retry = false;
            }
            if (fclose(file)) {
                err = ESP_FAIL;
                retry = false;
            }
            metrics.sync += factory_io_now() - started;
        }
        if (err == ESP_OK && preview) {
            uint8_t digest[32];
            size_t n = 0;
            char sha[65];
            if (!ideas_multipart_complete(preview))
                err = ESP_ERR_INVALID_ARG;
            else if (psa_hash_finish(&hash, digest, sizeof(digest), &n) != PSA_SUCCESS || n != 32)
                err = ESP_FAIL;
            else {
                ideas_hex(digest, sha);
                if (memcmp(sha, headers.etag + 1, 64))
                err = ESP_ERR_INVALID_CRC;
        }
        }
        psa_hash_abort(&hash);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (err == ESP_OK) {
            if (size)
                *size = used;
            break;
        }
        if (destination)
            progress(ctx, -(int64_t)used);
        if (!retry || attempt == 3 || cancelled(ctx, request))
            break;
        uint64_t base = 1000U << attempt, delay = base + esp_random() % (base + 1);
        uint64_t required = retry_after_ms(headers.retry_after);
        if (required > delay)
            delay = required;
        int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
        if (remaining <= 0 || delay >= (uint64_t)remaining)
            break;
        /* Cancellation remains responsive while respecting Retry-After. */
        int64_t until = esp_timer_get_time() + delay * 1000;
        while (!cancelled(ctx, request) && esp_timer_get_time() < until)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    factory_io_report("http", path, &metrics, block ? block : memory, block ? IO_BYTES : capacity,
                      err);
    free(block);
    return err;
}

esp_err_t ideas_fetch_preview(ideas_context_t *ctx, const job_t *job, pending_t *pending) {
    factory_network_snapshot_t network;
    if (factory_network_get_snapshot(&network) != ESP_OK ||
        network.state != FACTORY_NETWORK_CONNECTED)
        return ESP_ERR_INVALID_STATE;
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (err != ESP_OK)
        return err;
    /* Version is bounded and URL-encoded; it is never taken from cloud data. */
    char version[97];
    size_t used = 0;
    const unsigned char *v = (const unsigned char *)esp_app_get_description()->version;
    const char *hex = "0123456789ABCDEF";
    for (; *v && used + 3 < sizeof(version); ++v) {
        if ((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') ||
            strchr(".-_", *v))
            version[used++] = *v;
        else {
            version[used++] = '%';
            version[used++] = hex[*v >> 4];
            version[used++] = hex[*v & 15];
        }
    }
    version[used] = 0;
    unsigned language = factory_locale_get();
    char path[256];
    int length = snprintf(path, sizeof(path),
                          "/api/v1/ideas/%s/"
                          "manifest.json?profile=esp-mosaico%%2Fv2&chip_id=32&"
                          "flash_size=%lu&recovery_version=%s&locale=%s",
                          job->code, (unsigned long)flash_size, version, language ? "en" : "zh-CN");
    if (length < 0 || length >= sizeof(path))
        return ESP_ERR_INVALID_SIZE;
    ideas_multipart_t preview = {.pending = pending, .code = job->code, .language = language};
    return fetch_http(ctx, job->request_id, path, NULL, IDEAS_PREVIEW_BYTES, NULL, NULL, true, true,
                      &preview);
}

esp_err_t ideas_fetch_file(const ideas_file_t *file, const char *path, void *argument) {
    ideas_context_t *ctx = ((pending_t *)argument)->owner;
    const pending_t *pending = argument;
    const icon_t *asset = NULL;
    for (size_t i = 0; pending && i < pending->package.file_count; ++i)
        if (!strcmp(file->name, pending->package.files[i].name))
            asset = &pending->assets[i];
    if (asset && asset->data) {
        FILE *cached = fopen(path, "wb");
        if (!cached)
            return ESP_FAIL;
        bool ok = fwrite(asset->data, 1, asset->size, cached) == asset->size;
        if (fflush(cached) || fsync(fileno(cached)))
            ok = false;
        if (fclose(cached))
            ok = false;
        if (ok) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            ctx->state->received += asset->size;
            xSemaphoreGive(ctx->lock);
        }
        return ok ? ESP_OK : ESP_FAIL;
    }
    return fetch_http(ctx, 0, file->path, NULL, file->size, NULL, path, false,
                      !strcmp(file->name, "firmware/manifest.json"), NULL);
}

esp_err_t ideas_fetch_asset(ideas_context_t *ctx, const job_t *job, pending_t *pending,
                            unsigned index, bool local) {
    if (ideas_cancelled(ctx, job->request_id))
        return ESP_ERR_INVALID_STATE;
    if (index >= pending->package.file_count)
        return ESP_ERR_INVALID_ARG;
    if (pending->assets[index].data && pending->assets[index].size)
        return ESP_OK;
    const ideas_file_t *file = &pending->package.files[index];
    icon_t *asset = &pending->assets[index];
    asset->data = heap_caps_malloc(file->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!asset->data)
        return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    size_t used = 0;
    if (local) {
        char path[IDEAS_PATH_BYTES];
        err = ideas_store_path(ROOT, pending->package.code, file->name, path);
        FILE *f = err == ESP_OK ? fopen(path, "rb") : NULL;
        if (!f)
            return ESP_ERR_NOT_FOUND;
        used = fread(asset->data, 1, file->size, f);
        if (used != file->size || ferror(f) || fgetc(f) != EOF)
            err = ESP_ERR_INVALID_SIZE;
        fclose(f);
    } else {
        err = fetch_http(ctx, job->request_id, file->path, asset->data, file->size, &used, NULL,
                         false, !strcmp(file->name, "firmware/manifest.json"), NULL);
    }
    uint8_t digest[32];
    size_t size = 0;
    char actual[65];
    if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS ||
                          psa_hash_compute(PSA_ALG_SHA_256, asset->data, used, digest,
                                           sizeof(digest), &size) != PSA_SUCCESS ||
                          size != 32))
        err = ESP_FAIL;
    if (err == ESP_OK) {
        ideas_hex(digest, actual);
        if (strcmp(actual, file->sha256))
            err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK)
        asset->size = used;
    return err;
}
