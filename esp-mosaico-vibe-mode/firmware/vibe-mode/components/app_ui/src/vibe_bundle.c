// SPDX-License-Identifier: Apache-2.0
#include "vibe_bundle.h"
#include "bundle_gsp.h"
#include "esp_heap_caps.h"
#include "miniz.h"

extern const uint8_t vibe_bundle_compressed[];
extern const size_t vibe_bundle_compressed_size, vibe_bundle_size;
static void *s_bundle;
esp_err_t vibe_bundle_open(esp_gsp_config_t *config)
{
    if (!config)
        return ESP_ERR_INVALID_ARG;
    if (s_bundle)
        return ESP_ERR_INVALID_STATE;
    s_bundle = heap_caps_aligned_alloc(64, vibe_bundle_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bundle)
        return ESP_ERR_NO_MEM;
    /* The convenience mem-to-mem API places an approximately 11 KiB decoder
     * on the caller's stack. Keep that separate from GSP initialization's
     * stack budget by allocating state and using the streaming ROM API. */
    tinfl_decompressor *decoder =
        heap_caps_malloc(sizeof(*decoder), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoder) {
        heap_caps_free(s_bundle);
        s_bundle = NULL;
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(decoder);
    size_t input_size = vibe_bundle_compressed_size, size = vibe_bundle_size;
    const tinfl_status status =
        tinfl_decompress(decoder, vibe_bundle_compressed, &input_size, s_bundle, s_bundle, &size,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(decoder);
    if (status != TINFL_STATUS_DONE || size != vibe_bundle_size ||
        input_size != vibe_bundle_compressed_size) {
        heap_caps_free(s_bundle);
        s_bundle = NULL;
        return ESP_ERR_INVALID_CRC;
    }
    *config = ESP_GSP_CONFIG_INIT();
    config->bundle = s_bundle;
    config->bundle_size = vibe_bundle_size;
    config->directories = gsp_bundle_component_directories(&config->directory_count);
    /* The immutable, embedded bundle remains in PSRAM throughout UI lifetime.
     * GSP verifies its normal container CRCs and all resource bounds. */
    return ESP_OK;
}
