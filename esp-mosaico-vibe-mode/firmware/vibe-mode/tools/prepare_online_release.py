#!/usr/bin/env python3
"""Validate a Vibe Mode app image and prepare the immutable BIN and stable feed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

SLOT_SIZE = 0x1C0000


def image_version(data):
    if not 288 <= len(data) <= SLOT_SIZE or data[0] != 0xE9 or not 1 <= data[1] <= 16:
        raise ValueError("Expected a Vibe Mode application BIN fitting the 1.75 MiB slot")
    if struct.unpack_from("<H", data, 12)[0] != 32:
        raise ValueError("Expected an ESP32-S31 image")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("Missing application description")
    version_bytes, project_bytes = data[48:80], data[80:112]
    if b"\0" not in version_bytes or b"\0" not in project_bytes:
        raise ValueError("Unterminated application identity")
    version = version_bytes.split(b"\0", 1)[0].decode("ascii")
    if project_bytes.split(b"\0", 1)[0] != b"vibe-mode" or not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("Expected project vibe-mode and a stable MAJOR.MINOR.PATCH version")
    if any(int(part) > 0xFFFFFFFF for part in version.split(".")):
        raise ValueError("Version component exceeds uint32")
    offset, checksum = 24, 0xEF
    for index in range(data[1]):
        if offset + 8 > len(data):
            raise ValueError("Truncated segment header")
        size = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 8
        if size % 4 or offset + size > len(data) or (index == 0 and size < 256):
            raise ValueError("Invalid segment length")
        for value in data[offset:offset + size]:
            checksum ^= value
        offset += size
    end = (offset + 16) & ~15
    if end > len(data) or data[end - 1] != checksum:
        raise ValueError("Image checksum mismatch")
    if data[23] != 1 or data[end:end + 32] != hashlib.sha256(data[:end]).digest():
        raise ValueError("Missing or invalid appended image SHA-256")
    if any(value != 0xFF for value in data[end + 32:]):
        raise ValueError("Unexpected trailing data; use the application BIN, not a merged image")
    return version


def prepare(image, output):
    data = image.read_bytes()
    version = image_version(data)
    filename = "esp-mosaico-vibe-mode-{}.bin".format(version)
    manifest = {
        "schema_version": 1, "product": "esp-mosaico-vibe-mode",
        "latest_version": version, "title": "Vibe Mode " + version,
        "summary": "Vibe Mode firmware release.",
        "firmware": {"file": filename, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()},
    }
    output.mkdir(parents=True, exist_ok=True)
    target = output / filename
    if target.exists() and target.read_bytes() != data:
        raise ValueError("A different image already uses this version; bump the firmware version")
    target.write_bytes(data)
    feed = output / "esp-mosaico-vibe-mode-stable.json"
    temporary = feed.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary.replace(feed)
    return target, feed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        for path in prepare(args.image, args.output_dir):
            print(path)
    except (ValueError, OSError) as error:
        parser.exit(1, "Release preparation failed: {}\n".format(error))
