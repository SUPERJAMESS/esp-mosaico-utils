// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "factory_io_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
typedef psa_hash_operation_t hash_state_t;
static bool begin(hash_state_t *state) {
    *state = psa_hash_operation_init();
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_setup(state, PSA_ALG_SHA_256) == PSA_SUCCESS;
}
static bool update(hash_state_t *s, const void *p, size_t n) {
    return psa_hash_update(s, p, n) == PSA_SUCCESS;
}
static bool finish(hash_state_t *s, unsigned char out[32]) {
    size_t n = 0;
    bool ok = psa_hash_finish(s, out, 32, &n) == PSA_SUCCESS && n == 32;
    psa_hash_abort(s);
    return ok;
}
static void abort_hash(hash_state_t *s) { psa_hash_abort(s); }
#else
#include <openssl/evp.h>
typedef EVP_MD_CTX *hash_state_t;
static bool begin(hash_state_t *s) {
    *s = EVP_MD_CTX_new();
    return *s && EVP_DigestInit_ex(*s, EVP_sha256(), NULL) == 1;
}
static bool update(hash_state_t *s, const void *p, size_t n) {
    return EVP_DigestUpdate(*s, p, n) == 1;
}
static bool finish(hash_state_t *s, unsigned char out[32]) {
    unsigned n = 0;
    bool ok = EVP_DigestFinal_ex(*s, out, &n) == 1 && n == 32;
    EVP_MD_CTX_free(*s);
    *s = NULL;
    return ok;
}
static void abort_hash(hash_state_t *s) {
    EVP_MD_CTX_free(*s);
    *s = NULL;
}
#endif
static void hexadecimal(const unsigned char digest[32], char out[65]) {
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}
bool ideas_hash_data(const void *data, size_t size, char out[65]) {
    hash_state_t state = {0};
    unsigned char digest[32];
    bool ok = begin(&state) && update(&state, data, size) && finish(&state, digest);
    abort_hash(&state);
    if (ok)
        hexadecimal(digest, out);
    return ok;
}
bool ideas_hash_file(const char *path, uint32_t size, const char *expected) {
    return ideas_hash_file_progress(path, size, expected, NULL, NULL) == ESP_OK;
}
esp_err_t ideas_hash_file_progress(const char *path, uint32_t size, const char *expected,
                              ideas_hash_progress_fn progress, void *ctx) {
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    FILE *file = fopen(path, "rb");
    esp_err_t err = file ? ESP_OK : errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    unsigned char *buffer = err == ESP_OK ? malloc(4096) : NULL, digest[32];
    hash_state_t state = {0};
    if (err == ESP_OK && !buffer) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && !begin(&state)) err = ESP_FAIL;
    uint64_t total = 0;
    if (err == ESP_OK && progress) err = progress(0, ctx);
    while (err == ESP_OK) {
        int64_t started = factory_io_now();
        size_t n = fread(buffer, 1, 4096, file);
        metrics.read += factory_io_now() - started;
        if (!n) {
            if (ferror(file)) err = ESP_FAIL;
            break;
        }
        total += n;
        started = factory_io_now();
        if (total > size) err = ESP_ERR_INVALID_SIZE;
        else if (!update(&state, buffer, n)) err = ESP_FAIL;
        metrics.hash += factory_io_now() - started;
        if (err == ESP_OK && progress)
            err = progress((uint32_t)total, ctx);
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    if (err == ESP_OK && total != size) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && !finish(&state, digest)) err = ESP_FAIL;
    abort_hash(&state);
    if (file && fclose(file) && err == ESP_OK) err = ESP_FAIL;
    char actual[65];
    if (err == ESP_OK) {
        hexadecimal(digest, actual);
        if (strcmp(actual, expected)) err = ESP_ERR_INVALID_CRC;
    }
    metrics.bytes = total;
    factory_io_report("nand_hash", path, &metrics, buffer, 4096, err);
    free(buffer);
    return err;
}
