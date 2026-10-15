"""Validate the standalone publication API and optional dependency boundary."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest
from PIL import Image

from shared.ideas import generate_resources
from shared.ideas import presentation


@pytest.fixture
def inputs(monkeypatch, tmp_path):
    class Font:
        def __init__(self, path, pixels, locale):
            self.pixels = pixels

        def layout(self, text, width, height):
            return dict(text=text, width=width, height=height, glyphs=[])

        def encode(self):
            return bytes([self.pixels])

    monkeypatch.setattr(presentation, "Font", Font)
    return dict(name="Weather", description="Clear skies.", locale="en",
                font=tmp_path / "caller-font.ttf")


def test_resource_generation_has_no_filesystem_output_and_owns_its_bytes(inputs, tmp_path):
    first = generate_resources(**inputs)
    assert first == generate_resources(**inputs)
    assert set(first) == {"display.json", "title.mif", "body.mif"}
    display = json.loads(first["display.json"])
    assert display["schema"] == "mosaico-ideas/display/1"
    assert display["name"]["font"] == "title.mif"
    assert display["icon"] == ""
    assert not list(tmp_path.iterdir())
    first["display.json"] = b"changed by caller"
    assert generate_resources(**inputs)["display.json"] != first["display.json"]


@pytest.mark.parametrize("field,value", [
    ("name", ""), ("name", "界" * 43), ("description", "a" * 513),
    ("name", "hidden\x00text"), ("locale", ""), ("locale", "中文"),
    ("locale", "a" * 24),
])
def test_device_limits_are_checked_before_rendering(inputs, field, value):
    inputs[field] = value
    with pytest.raises(ValueError):
        generate_resources(**inputs)


def test_icon_normalization_and_animation_rejection(inputs, tmp_path):
    icon = tmp_path / "source.png"
    Image.new("RGB", (128, 80), "red").save(icon)
    inputs["icon"] = icon
    before = icon.read_bytes()
    resources = generate_resources(**inputs)
    import io
    with Image.open(io.BytesIO(resources["icon.png"])) as normalized:
        assert normalized.size == (64, 64)
        assert normalized.mode == "RGBA"
    assert icon.read_bytes() == before
    animated = tmp_path / "animated.gif"
    Image.new("RGB", (8, 8), "red").save(animated, save_all=True,
        append_images=[Image.new("RGB", (8, 8), "blue")], duration=100)
    inputs["icon"] = animated
    with pytest.raises(ValueError, match="static image"):
        generate_resources(**inputs)


def test_api_import_needs_no_consumer_or_rendering_dependencies(tmp_path):
    root = Path(__file__).resolve().parents[2]
    shutil.copytree(root, tmp_path / "shared", ignore=shutil.ignore_patterns("__pycache__"))
    code = (
        "import sys; from shared.ideas import generate_resources; "
        "assert callable(generate_resources); "
        "assert not any(n.startswith(('PIL', 'freetype', 'mosaico_cli')) for n in sys.modules)"
    )
    result = subprocess.run([sys.executable, "-S", "-c", code], cwd=tmp_path,
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
