"""Validate the immutable lightweight pack and all glyph/image bounds."""

import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "firmware/vibe-mode"


def test_embedded_assets_match_sources_and_have_bounded_records():
    subprocess.run(
        [sys.executable, str(ROOT / "tools/check_vui_assets.py")], check=True
    )
    packed = (ROOT / "components/app_ui/light/vui_assets.bin").read_bytes()
    raw = zlib.decompress(packed[4:])
    assert len(raw) == struct.unpack_from("<I", packed)[0]
    magic, size, nodes, count, fonts, font_count, images, image_count = (
        struct.unpack_from("<8I", raw)
    )
    assert magic == 0x31554956 and size == len(raw)
    assert count < 2048 and nodes + count * 44 <= size
    assert fonts + font_count * 16 <= size and images + image_count * 12 <= size
    for i in range(count):
        node = struct.unpack_from("<5h15HI", raw, nodes + i * 44)
        (
            parent,
            _x,
            _y,
            w,
            h,
            kind,
            _flags,
            _radius,
            _bg,
            _fg,
            _border,
            _border_width,
            font,
            _align,
            _action,
            _arg,
            image,
            template,
            height,
            _reserved,
            text,
        ) = node
        assert -1 <= parent < count and parent != i
        assert w >= 0 and h >= 0 and kind < 9 and font < font_count
        if kind == 8:  # Procedural dots have no bitmap frames.
            assert image == 0
        assert image <= image_count and template <= count and text < size
        assert b"\0" in raw[text:]
        seen = {i}
        while parent >= 0:
            assert parent not in seen
            seen.add(parent)
            parent = struct.unpack_from("<h", raw, nodes + parent * 44)[0]
    for i in range(font_count):
        glyphs, n, _ascent, _descent, height, _ = struct.unpack_from(
            "<II4h", raw, fonts + i * 16
        )
        assert glyphs + n * 20 <= size and height > 0
        previous = -1
        for g in range(n):
            code, data, _x, _y, w, h, advance, _ = struct.unpack_from(
                "<II6h", raw, glyphs + g * 20
            )
            assert code > previous and w >= 0 and h >= 0 and advance >= 0
            assert data + w * h <= size
            previous = code
    for i in range(image_count):
        data, w, h, frames, duration = struct.unpack_from("<I4H", raw, images + i * 12)
        assert w > 0 and h > 0 and frames > 0 and duration > 0
        assert data + w * h * frames * 3 <= size
    # Include bilingual firmware-update pages; the full image still has its fixed slot check.
    assert len(packed) < 256 * 1024
    assert len(raw) < 1152 * 1024
