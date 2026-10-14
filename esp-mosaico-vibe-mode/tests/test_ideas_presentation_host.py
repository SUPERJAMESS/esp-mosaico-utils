"""Exercise the S31 PNG inflate adapter on the host."""
from pathlib import Path
import shutil
import subprocess


MAIN = Path(__file__).resolve().parents[1] / "firmware/vibe-mode/components/app_ui/src"


def compile_run(tmp_path, sources, program):
    test = tmp_path / "test.c"
    test.write_text(program)
    executable = tmp_path / "test"
    result = subprocess.run([shutil.which("cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(tmp_path), "-I" + str(MAIN),
                    *[str(MAIN / source) for source in sources], str(test),
                    "-o", str(executable)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(executable)], cwd=tmp_path, check=True, timeout=5)


def test_png_linear_buffer_fix_preserves_streaming_calls(tmp_path):
    (tmp_path / "miniz.h").write_text("""
#include <stddef.h>
typedef unsigned char mz_uint8;
typedef unsigned int mz_uint32;
typedef int tinfl_status;
typedef struct { int state; } tinfl_decompressor;
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_HAS_MORE_INPUT 2
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
""")
    compile_run(tmp_path, ["vibe_png_inflate.c"], r"""
#include "miniz.h"
#include <assert.h>
static mz_uint32 observed;
tinfl_status __real_tinfl_decompress(tinfl_decompressor *d, const mz_uint8 *in, size_t *in_size, mz_uint8 *start, mz_uint8 *next, size_t *out_size, mz_uint32 flags) { (void)d; (void)in; (void)in_size; (void)start; (void)next; (void)out_size; observed = flags; return 123; }
tinfl_status __wrap_tinfl_decompress(tinfl_decompressor *, const mz_uint8 *, size_t *, mz_uint8 *, mz_uint8 *, size_t *, mz_uint32);
int main(void) {
    mz_uint8 buffer[2]; size_t output = 16448, input = 50;
    assert(__wrap_tinfl_decompress(NULL, NULL, &input, buffer, buffer, &output, 1) == 123);
    assert(observed == 5);
    __wrap_tinfl_decompress(NULL, NULL, &input, buffer, buffer, &output, 3); assert(observed == 3);
    __wrap_tinfl_decompress(NULL, NULL, &input, buffer, buffer+1, &output, 1); assert(observed == 1);
    output = 32768;
    __wrap_tinfl_decompress(NULL, NULL, &input, buffer, buffer, &output, 1); assert(observed == 1);
    output = 0;
    __wrap_tinfl_decompress(NULL, NULL, &input, buffer, buffer, &output, 1); assert(observed == 1);
    return 0;
}
""")
