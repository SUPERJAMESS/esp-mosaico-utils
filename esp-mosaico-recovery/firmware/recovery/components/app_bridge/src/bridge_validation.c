// SPDX-License-Identifier: Apache-2.0
#include "bridge_internal.h"

const char *bridge_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

uint32_t bridge_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX
               ? (uint32_t)v->valuedouble
               : 0;
}

void bridge_hex(const uint8_t *in, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = h[in[i] >> 4];
        out[2 * i + 1] = h[in[i] & 15];
    }
    out[n * 2] = 0;
}

bool bridge_digest(const void *p, size_t n, uint8_t out[32])
{
    size_t written = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, p, n, out, 32, &written) == PSA_SUCCESS &&
           written == 32;
}

bool bridge_matches(const void *p, size_t n, const char *expected)
{
    uint8_t d[32];
    char h[65];
    if (!bridge_digest(p, n, d))
        return false;
    bridge_hex(d, 32, h);
    return strcmp(h, expected) == 0;
}

bool bridge_flash_hash(uint32_t offset, uint32_t size, char out[65])
{
    uint8_t buf[1024], hash[32];
    size_t written = 0;
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return false;
    for (uint32_t pos = 0; pos < size;) {
        size_t n = size - pos;
        if (n > sizeof(buf))
            n = sizeof(buf);
        if (esp_flash_read(NULL, buf, offset + pos, n) != ESP_OK ||
            psa_hash_update(&h, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&h);
            return false;
        }
        pos += n;
    }
    if (psa_hash_finish(&h, hash, 32, &written) != PSA_SUCCESS || written != 32) {
        psa_hash_abort(&h);
        return false;
    }
    bridge_hex(hash, 32, out);
    return true;
}
