// SPDX-License-Identifier: Apache-2.0
#include "ideas_package.h"
#include "factory_io_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    return ideas_hash_file_progress(path, size, expected, NULL, NULL);
}
bool ideas_hash_file_progress(const char *path, uint32_t size, const char *expected,
                              ideas_hash_progress_fn progress, void *ctx) {
    factory_io_metrics_t metrics = {.started = factory_io_now()};
    FILE *file = fopen(path, "rb");
    unsigned char *buffer = malloc(4096), digest[32];
    hash_state_t state = {0};
    bool ok = file && buffer && begin(&state);
    uint64_t total = 0;
    while (ok) {
        int64_t started = factory_io_now();
        size_t n = fread(buffer, 1, 4096, file);
        metrics.read += factory_io_now() - started;
        if (!n)
            break;
        total += n;
        started = factory_io_now();
        ok = total <= size && update(&state, buffer, n);
        metrics.hash += factory_io_now() - started;
        if (ok && progress)
            progress((uint32_t)total, ctx);
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    ok = ok && total == size && !ferror(file) && finish(&state, digest);
    abort_hash(&state);
    if (file)
        fclose(file);
    char actual[65];
    if (ok)
        hexadecimal(digest, actual);
    ok = ok && !strcmp(actual, expected);
    metrics.bytes = total;
    factory_io_report("nand_hash", path, &metrics, buffer, 4096, ok ? 0 : -1);
    free(buffer);
    return ok;
}
