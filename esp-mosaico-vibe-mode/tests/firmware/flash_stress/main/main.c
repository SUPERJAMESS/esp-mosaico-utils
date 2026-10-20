// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_iris.h"
#include "esp_partition.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "stress_inputs.h"

#define STRESS_SERVICE_ID 0x6a03U

extern const uint8_t payload_start[] asm("_binary_payload_bin_start");
extern const uint8_t payload_end[] asm("_binary_payload_bin_end");

/* Immutable after boot; Iris owns the RPC callback lifetime until reset. */
typedef struct {
    size_t payload_size;
    char payload[65];
    char resource_a[65];
    char resource_b[65];
    char ui_apps[65];
} stress_evidence_t;

static void hex_digest(const uint8_t digest[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 15];
    }
    output[64] = 0;
}

static esp_err_t hash_partition(const char *label, const char *expected, char output[65])
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) return ESP_ERR_NOT_FOUND;
    uint8_t digest[32];
    esp_err_t err = esp_partition_get_sha256(partition, digest);
    if (err != ESP_OK) return err;
    hex_digest(digest, output);
    return expected != NULL && strcmp(output, expected) != 0 ? ESP_ERR_INVALID_CRC : ESP_OK;
}

static esp_err_t evidence_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                               size_t capacity, size_t *size, void *user_ctx)
{
    const stress_evidence_t *evidence = user_ctx;
    *size = 0;
    if (request->payload_size != 0) return ESP_ERR_INVALID_ARG;
    int count = snprintf((char *)response, capacity,
        "{\"sample\":\"%s\",\"payload_size\":%u,\"payload_sha256\":\"%s\","
        "\"stress_a_sha256\":\"%s\",\"stress_b_sha256\":\"%s\",\"ui_apps_sha256\":\"%s\"}",
        STRESS_SAMPLE, (unsigned)evidence->payload_size, evidence->payload,
        evidence->resource_a, evidence->resource_b, evidence->ui_apps);
    if (count < 0 || (size_t)count >= capacity) return ESP_ERR_INVALID_SIZE;
    *size = (size_t)count;
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_boot_probe());
    /* Never erase an NVS partition to resolve an initialization error. */
    ESP_ERROR_CHECK(nvs_flash_init());
    stress_evidence_t *evidence = calloc(1, sizeof(*evidence));
    ESP_ERROR_CHECK(evidence == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    evidence->payload_size = (size_t)(payload_end - payload_start);
    uint8_t digest[32];
    size_t digest_size = 0;
    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(psa_hash_compute(PSA_ALG_SHA_256, payload_start, evidence->payload_size,
                                    digest, sizeof(digest), &digest_size) == PSA_SUCCESS &&
                            digest_size == sizeof(digest) ? ESP_OK : ESP_FAIL);
    hex_digest(digest, evidence->payload);
    ESP_ERROR_CHECK(strcmp(evidence->payload, STRESS_PAYLOAD_SHA256) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC);
    ESP_ERROR_CHECK(hash_partition("ui_apps", NULL, evidence->ui_apps));
    if (STRESS_STRESS_A_SHA256[0]) {
        ESP_ERROR_CHECK(hash_partition("stress_a", STRESS_STRESS_A_SHA256, evidence->resource_a));
        ESP_ERROR_CHECK(hash_partition("stress_b", STRESS_STRESS_B_SHA256, evidence->resource_b));
    }
    ESP_ERROR_CHECK(esp_iris_rpc_register(STRESS_SERVICE_ID, 1, evidence_rpc, evidence));
    /* Readback verification precedes the adapter's delayed HEALTHY event. */
    iris_ota_support_start();
}
