#!/usr/bin/env python3
"""Generate the 0.2 Recovery installer otadata; never used by native app flash."""
from pathlib import Path
import argparse

BOOTSTRAP_OFFSET = 32
BOOTSTRAP_MARKER = b"MOSAICO-BOOT-02!"
OTADATA_BYTES = 0x2000


def bootstrap_image() -> bytes:
    image = bytearray(b"\xff" * OTADATA_BYTES)
    image[BOOTSTRAP_OFFSET:BOOTSTRAP_OFFSET + len(BOOTSTRAP_MARKER)] = BOOTSTRAP_MARKER
    return bytes(image)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bootstrap_image())
