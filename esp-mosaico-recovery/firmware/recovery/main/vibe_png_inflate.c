// SPDX-License-Identifier: Apache-2.0
#include "miniz.h"

tinfl_status __real_tinfl_decompress(tinfl_decompressor *decoder,
    const mz_uint8 *input, size_t *input_size, mz_uint8 *output_start,
    mz_uint8 *output_next, size_t *output_size, mz_uint32 flags);

tinfl_status __wrap_tinfl_decompress(tinfl_decompressor *decoder,
    const mz_uint8 *input, size_t *input_size, mz_uint8 *output_start,
    mz_uint8 *output_next, size_t *output_size, mz_uint32 flags)
{
    /* GSP 1.4's PNG decoder supplies a complete zlib stream and an exact-size
     * image buffer, but omits NON_WRAPPING. S31 ROM rejects a non-power-of-two
     * dictionary before decoding (64x64 RGBA needs 16448 filtered bytes).
     * Correct only that otherwise-invalid initial call. Valid circular
     * dictionaries, incremental streams and explicitly linear calls retain
     * their original flags. */
    if (flags == TINFL_FLAG_PARSE_ZLIB_HEADER && output_start == output_next &&
        output_size && *output_size && (*output_size & (*output_size - 1)))
        flags |= TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
    return __real_tinfl_decompress(decoder, input, input_size, output_start,
                                   output_next, output_size, flags);
}
