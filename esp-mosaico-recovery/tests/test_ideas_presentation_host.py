"""Exercise the real card lifecycle and S31 PNG inflate adapter on the host."""
from pathlib import Path
import shutil
import subprocess


MAIN = Path(__file__).resolve().parents[1] / "firmware/recovery/main"


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


def test_card_consumes_remote_input_until_hub_resumes(tmp_path):
    sdk = r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef int esp_err_t;
typedef void *esp_gsp_handle_t;
typedef struct { int unused; } esp_gsp_config_t;
typedef struct { int unused; } esp_gsp_deployable_bundle_t;
typedef struct { int unused; } esp_gsp_esp_lcd_session_t;
typedef struct { size_t struct_size; unsigned scene_count, dynamic_image_slots; } esp_gsp_deployable_info_t;
typedef bool (*interceptor_t)(esp_gsp_handle_t, int32_t, int32_t, bool, void *);
#define ESP_OK 0
#define ESP_GSP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NOT_SUPPORTED 3
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned ticks);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned flags);
void heap_caps_free(void *data);
esp_err_t esp_gsp_set_input_interceptor(esp_gsp_handle_t ui, interceptor_t cb, void *ctx);
esp_err_t esp_gsp_deployable_bundle_open(void *data, size_t size, bool crc, esp_gsp_deployable_bundle_t **out);
esp_err_t esp_gsp_deployable_bundle_get_info(esp_gsp_deployable_bundle_t *bundle, esp_gsp_deployable_info_t *info);
esp_err_t esp_gsp_deployable_bundle_make_config(esp_gsp_deployable_bundle_t *bundle, esp_gsp_config_t *config);
void esp_gsp_deployable_bundle_close(esp_gsp_deployable_bundle_t *bundle);
esp_err_t esp_gsp_esp_lcd_suspend(esp_gsp_handle_t hub, esp_gsp_esp_lcd_session_t **out);
esp_err_t esp_gsp_esp_lcd_start_on_session_prepared(esp_gsp_esp_lcd_session_t *session, const esp_gsp_config_t *config, void (*prepare)(esp_gsp_handle_t, void *), void *ctx, esp_gsp_handle_t *out);
esp_err_t esp_gsp_esp_lcd_resume(esp_gsp_esp_lcd_session_t *session, esp_gsp_handle_t app, esp_gsp_handle_t *hub);
"""
    (tmp_path / "sdk.h").write_text(sdk)
    for name in ("esp_gsp.h", "esp_err.h", "esp_gsp_deployable.h", "esp_gsp_esp_lcd.h",
                 "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/task.h"):
        header = tmp_path / name
        header.parent.mkdir(exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    (tmp_path / "card.gspb").write_bytes(b"verified card")
    compile_run(tmp_path, ["ideas_presentation.c"], r"""
#include "sdk.h"
#include "ideas_presentation.h"
#include <assert.h>
static int resume_calls, closes, freed, fail_start;
static bool physical;
static interceptor_t input;
static void *input_ctx;
static esp_gsp_deployable_bundle_t bundle;
static esp_gsp_esp_lcd_session_t session;
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned flags) { (void)alignment; (void)flags; return malloc(size); }
void heap_caps_free(void *data) { ++freed; free(data); }
esp_err_t esp_gsp_deployable_bundle_open(void *data, size_t size, bool crc, esp_gsp_deployable_bundle_t **out) { assert(data && size && crc); *out = &bundle; return 0; }
esp_err_t esp_gsp_deployable_bundle_get_info(esp_gsp_deployable_bundle_t *b, esp_gsp_deployable_info_t *info) { assert(b == &bundle); info->scene_count = 1; info->dynamic_image_slots = 0; return 0; }
esp_err_t esp_gsp_deployable_bundle_make_config(esp_gsp_deployable_bundle_t *b, esp_gsp_config_t *config) { (void)b; config->unused = 0; return 0; }
void esp_gsp_deployable_bundle_close(esp_gsp_deployable_bundle_t *b) { assert(b == &bundle); ++closes; }
esp_err_t esp_gsp_set_input_interceptor(esp_gsp_handle_t ui, interceptor_t cb, void *ctx) { assert(ui == (void *)2); input = cb; input_ctx = ctx; return 0; }
esp_err_t esp_gsp_esp_lcd_suspend(esp_gsp_handle_t hub, esp_gsp_esp_lcd_session_t **out) { assert(hub == (void *)1); assert(ideas_presentation_input(100, 100, false)); *out = &session; return 0; }
esp_err_t esp_gsp_esp_lcd_start_on_session_prepared(esp_gsp_esp_lcd_session_t *s, const esp_gsp_config_t *config, void (*prepare)(esp_gsp_handle_t, void *), void *ctx, esp_gsp_handle_t *out) { (void)config; assert(s == &session); if (fail_start) return ESP_FAIL; *out = (void *)2; prepare(*out, ctx); return 0; }
void vTaskDelay(unsigned ticks) {
    if (ticks == 100) return;
    assert(ticks == 50 && input);
    /* An unpaired release and ordinary touches must not close the card. */
    assert(ideas_presentation_input(20, 20, false));
    assert(ideas_presentation_input(200, 200, true));
    assert(ideas_presentation_input(200, 200, false));
    if (physical) { input((void *)2, 20, 20, true, input_ctx); input((void *)2, 20, 20, false, input_ctx); }
    else { assert(ideas_presentation_input(20, 20, true)); assert(ideas_presentation_input(20, 20, false)); }
}
esp_err_t esp_gsp_esp_lcd_resume(esp_gsp_esp_lcd_session_t *s, esp_gsp_handle_t app, esp_gsp_handle_t *hub) { assert(s == &session && app == (void *)2); assert(ideas_presentation_input(200, 200, false)); if (++resume_calls % 2) return ESP_FAIL; *hub = (void *)1; return 0; }
int main(void) {
    assert(!ideas_presentation_input(20, 20, true));
    for (unsigned i = 0; i < 2; ++i) {
        physical = i != 0;
        assert(ideas_presentation_show((void *)1, "card.gspb") == ESP_OK);
        assert(!ideas_presentation_input(20, 20, false));
    }
    assert(resume_calls == 4 && closes == 2 && freed == 2);
    fail_start = 1;
    assert(ideas_presentation_show((void *)1, "card.gspb") == ESP_FAIL);
    assert(!ideas_presentation_input(20, 20, true));
    assert(closes == 3 && freed == 3);
    return 0;
}
""")


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
