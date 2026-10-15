"""Validate upload artifacts and reject mixed, corrupted or overwritten releases."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "firmware/vibe-mode/tools/prepare_online_release.py"
spec = importlib.util.spec_from_file_location("online_release", SCRIPT)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def image(version="0.2.1", project="vibe-mode", chip=32):
    data = bytearray(304)
    data[0], data[1], data[23] = 0xE9, 1, 1
    struct.pack_into("<H", data, 12, chip)
    struct.pack_into("<I", data, 28, 256)
    struct.pack_into("<I", data, 32, 0xABCD5432)
    data[48:48 + len(version)] = version.encode()
    data[80:80 + len(project)] = project.encode()
    checksum = 0xEF
    for value in data[32:288]:
        checksum ^= value
    data[303] = checksum
    return bytes(data) + hashlib.sha256(data).digest()


def test_release_matches_image_and_cannot_overwrite_version(tmp_path):
    source = tmp_path / "vibe-mode.bin"
    source.write_bytes(image())
    binary, feed = release.prepare(source, tmp_path / "release")
    manifest = json.loads(feed.read_text())
    assert binary.name == "esp-mosaico-vibe-mode-0.2.1.bin"
    assert manifest["latest_version"] == "0.2.1"
    assert manifest["firmware"] == {"file": binary.name, "size": len(image()), "sha256": hashlib.sha256(image()).hexdigest()}
    assert binary.read_bytes() == source.read_bytes()
    release.prepare(source, binary.parent)
    binary.write_bytes(b"different image")
    with pytest.raises(ValueError, match="already uses this version"):
        release.prepare(source, binary.parent)


@pytest.mark.parametrize("data", [image(project="normal-app"), image(version="0.2.1-dev"),
                                  image(chip=9), image()[:-1], image() + b"not padding",
                                  b"\0" * 4096, image()[:300]],
                         ids=["wrong-project", "development-version", "wrong-chip", "truncated-hash",
                              "trailing-data", "invalid-header", "truncated-image"])
def test_reject_invalid_release_images(data):
    with pytest.raises(ValueError):
        release.image_version(data)
