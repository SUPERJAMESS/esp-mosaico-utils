"""Exercise the lightweight UI's bounded PNG decoder with real encoded images."""

import ctypes
import io
import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest
from PIL import Image

UI = Path(__file__).resolve().parents[1] / "firmware/vibe-mode/components/app_ui"


@pytest.fixture(scope="module")
def decoder(tmp_path_factory):
    directory = tmp_path_factory.mktemp("vui-png")
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("A C compiler is required")
    (directory / "esp_err.h").write_text("typedef int esp_err_t;\n")
    output = directory / ("decode.dll" if os.name == "nt" else "decode.so")
    # Release in the same C runtime that allocated the decoded buffer.
    bridge = directory / "bridge.c"
    bridge.write_text("#include <stdlib.h>\nvoid decoded_free(void *p) { free(p); }\n")
    build = subprocess.run(
        [
            cc,
            "-shared",
            "-fPIC",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(UI / "private"),
            "-I" + str(UI.parent / "app_ideas/private"),
            "-I" + str(UI.parent / "app_ideas/include"),
            "-I" + str(directory),
            str(UI.parent / "app_ideas/src/ideas_title.c"),
            str(UI / "light/vui_decode.c"),
            str(bridge),
            "-lz",
            "-o",
            str(output),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    dll_directory = (
        os.add_dll_directory(str(Path(cc).parent)) if os.name == "nt" else None
    )
    lib = ctypes.CDLL(str(output))
    lib.vui_png.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint),
    ]
    lib.vui_png.restype = ctypes.c_void_p
    lib.decoded_free.argtypes = [ctypes.c_void_p]
    lib.vui_title_qoi.argtypes = lib.vui_png.argtypes
    lib.vui_title_qoi.restype = ctypes.c_void_p
    lib.ideas_title_qoi.argtypes = [ctypes.POINTER(ctypes.c_uint16), ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_size_t)]
    lib.ideas_title_qoi.restype = ctypes.c_void_p

    def decode(data, title=False):
        w = ctypes.c_uint()
        h = ctypes.c_uint()
        p = (lib.vui_title_qoi if title else lib.vui_png)(data, len(data), ctypes.byref(w), ctypes.byref(h))
        if not p:
            return None
        try:
            return w.value, h.value, ctypes.string_at(p, w.value * h.value * 3)
        finally:
            lib.decoded_free(p)

    def encode_title(pixels, width=328, height=84):
        size = ctypes.c_size_t()
        p = lib.ideas_title_qoi(pixels, width, height, ctypes.byref(size))
        assert p
        try: return ctypes.string_at(p, size.value)
        finally: lib.decoded_free(p)
    decode.encode_title = encode_title
    yield decode
    if dll_directory:
        dll_directory.close()


@pytest.mark.parametrize("mode", ["1", "L", "LA", "RGB", "RGBA", "P"])
def test_png_modes_preserve_pixels_and_alpha(decoder, mode):
    image = Image.new("RGBA", (19, 13))
    image.putdata(
        [
            (x * 11 % 256, y * 17 % 256, (x + y) * 7 % 256, (x * 23 + y * 31) % 256)
            for y in range(13)
            for x in range(19)
        ]
    )
    image = image.convert(mode)
    if mode == "P":
        image.info["transparency"] = bytes(range(256))
    encoded = io.BytesIO()
    image.save(encoded, format="PNG")
    w, h, pixels = decoder(encoded.getvalue())
    assert (w, h) == image.size
    for i, (r, g, b, a) in enumerate(image.convert("RGBA").getdata()):
        c = pixels[i * 3] | pixels[i * 3 + 1] << 8
        assert c == ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        assert pixels[i * 3 + 2] == a


def test_png_rejects_corruption_truncation_and_oversize(decoder):
    image = Image.new("RGBA", (16, 16), (255, 80, 1, 128))
    output = io.BytesIO()
    image.save(output, format="PNG")
    data = output.getvalue()
    for length in (0, 7, 24, len(data) - 1):
        assert decoder(data[:length]) is None
    bad = bytearray(data)
    bad[-20] ^= 0x40
    assert decoder(bytes(bad)) is None
    bad = bytearray(data)
    struct.pack_into(">I", bad, 16, 257)
    struct.pack_into(">I", bad, 29, zlib.crc32(bad[12:29]))
    assert decoder(bytes(bad)) is None
    assert decoder(data + b"extra") is None


@pytest.mark.parametrize("filter_type", range(5))
def test_all_png_scanline_filters(decoder, filter_type):
    def chunk(name, data):
        return (
            struct.pack(">I", len(data))
            + name
            + data
            + struct.pack(">I", zlib.crc32(name + data))
        )

    rows = [bytes((x * 27 + y * 33) % 256 for x in range(9)) for y in range(4)]
    encoded = bytearray()
    for y, row in enumerate(rows):
        encoded.append(filter_type)
        for x, value in enumerate(row):
            a = row[x - 3] if x >= 3 else 0
            b = rows[y - 1][x] if y else 0
            c = rows[y - 1][x - 3] if y and x >= 3 else 0
            p = a + b - c
            choices = [a, b, c]
            paeth = min(choices, key=lambda v: abs(p - v))
            prediction = [0, a, b, (a + b) // 2, paeth][filter_type]
            encoded.append((value - prediction) & 255)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 3, 4, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(encoded))
        + chunk(b"IEND", b"")
    )
    w, h, out = decoder(png)
    assert (w, h) == (3, 4)
    for i in range(12):
        r, g, b = rows[i // 3][i % 3 * 3 : i % 3 * 3 + 3]
        assert out[i * 3] | out[i * 3 + 1] << 8 == ((r >> 3) << 11) | (
            (g >> 2) << 5
        ) | (b >> 3)
        assert out[i * 3 + 2] == 255


@pytest.mark.parametrize("interlace", [0, 1])
@pytest.mark.parametrize(
    "color,depth",
    [
        (0, 1),
        (0, 2),
        (0, 4),
        (0, 8),
        (0, 16),
        (2, 8),
        (2, 16),
        (3, 1),
        (3, 2),
        (3, 4),
        (3, 8),
        (4, 8),
        (4, 16),
        (6, 8),
        (6, 16),
    ],
)
@pytest.mark.parametrize("dimensions", [(1, 2), (13, 9)])
def test_png_bit_depths_and_adam7_preserve_existing_icon_contract(
    decoder, color, depth, interlace, dimensions
):
    def chunk(name, data):
        return (
            struct.pack(">I", len(data))
            + name
            + data
            + struct.pack(">I", zlib.crc32(name + data))
        )

    w, h = dimensions
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    mask = (1 << depth) - 1
    samples = [
        tuple((x * 73 + y * 111 + c * 87) & mask for c in range(channels))
        for y in range(h)
        for x in range(w)
    ]
    # Published pass-number pattern, independent of the decoder's stride table.
    pattern = [
        [1, 6, 4, 6, 2, 6, 4, 6],
        [7] * 8,
        [5, 6, 5, 6, 5, 6, 5, 6],
        [7] * 8,
        [3, 6, 4, 6, 3, 6, 4, 6],
        [7] * 8,
        [5, 6, 5, 6, 5, 6, 5, 6],
        [7] * 8,
    ]
    raw = bytearray()
    for passno in range(1, 8) if interlace else [0]:
        previous = None
        for y in range(h):
            values = [
                v
                for x in range(w)
                if not interlace or pattern[y % 8][x % 8] == passno
                for v in samples[y * w + x]
            ]
            if not values:
                continue
            bits = "".join(format(v, f"0{depth}b") for v in values)
            bits += "0" * (-len(bits) % 8)
            row = bytes(int(bits[i : i + 8], 2) for i in range(0, len(bits), 8))
            # Use Up so the reference also tests resetting the prior row per pass.
            raw.append(2)
            raw.extend(
                (v - (previous[i] if previous else 0)) & 255 for i, v in enumerate(row)
            )
            previous = row
    palette = (
        [(i, (255 - i), i * 3 % 256) for i in range(1 << depth)] if color == 3 else []
    )
    png = b"\x89PNG\r\n\x1a\n" + chunk(
        b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, interlace)
    )
    if palette:
        png += chunk(b"PLTE", bytes(v for pixel in palette for v in pixel)) + chunk(
            b"tRNS", bytes(range(len(palette)))
        )
    if color in (0, 2):
        png += chunk(b"tRNS", b"".join(struct.pack(">H", v) for v in samples[0]))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    ow, oh, data = decoder(png)
    assert (ow, oh) == (w, h)
    for i, values in enumerate(samples):
        scaled = [v >> 8 if depth == 16 else v * 255 // mask for v in values]
        if color == 3:
            r, g, b = palette[values[0]]
            a = values[0]
        elif color in (0, 4):
            r = g = b = scaled[0]
            a = scaled[1] if color == 4 else 0 if values == samples[0] else 255
        else:
            r, g, b = scaled[:3]
            a = scaled[3] if color == 6 else 0 if values == samples[0] else 255
        assert data[i * 3] | data[i * 3 + 1] << 8 == ((r >> 3) << 11) | (
            (g >> 2) << 5
        ) | (b >> 3)
        assert data[i * 3 + 2] == a


def test_local_title_cache_preserves_glyphs_and_rejects_malformed_data(decoder):
    pixels = (ctypes.c_uint16 * (328 * 84))()
    pixels[10 * 328 + 7] = 0xffff
    pixels[12 * 328 + 8] = 0xfa60
    raw = decoder.encode_title(pixels)
    width, height, decoded = decoder(raw, title=True)
    assert (width, height) == (328, 3)
    for y in range(height):
        for x in range(width):
            at = (y * width + x) * 3
            color = pixels[(y + 10) * width + x]
            assert decoded[at] | (decoded[at+1] << 8) == color
            assert decoded[at+2] == (255 if color else 0)
    for end in (0, 13, 20, len(raw)-1):
        assert decoder(raw[:end], title=True) is None
    assert decoder(raw + b"extra", title=True) is None
    assert decoder(raw[:14] + b"\xfd" * 445 + raw[-8:], title=True) is None
    bad = bytearray(raw); bad[14] = 0xff
    assert decoder(bytes(bad), title=True) is None


def test_wide_title_cache_keeps_native_dimensions_and_rejects_unbounded_sizes(decoder):
    pixels = (ctypes.c_uint16 * (640 * 28))(*([0xffff] * (640 * 28)))
    raw = decoder.encode_title(pixels, 640, 28)
    w, h, data = decoder(raw, title=True)
    assert (w, h) == (640, 28) and len(data) == 640 * 28 * 3
    for w, h in [(0,28), (4097,1), (328,85), (4096,84)]:
        invalid = bytearray(raw)
        struct.pack_into(">II", invalid, 4, w, h)
        assert decoder(bytes(invalid), title=True) is None
