"""Embed the checked-in asset pack using portable C (including MSVC)."""

import sys
from pathlib import Path

source, target = map(Path, sys.argv[1:])
data = source.read_bytes()
lines = ["#include <stdint.h>", "const uint8_t vui_assets_start[] = {"]
lines += [
    ",".join(str(b) for b in data[i : i + 32]) + "," for i in range(0, len(data), 32)
]
lines += ["};", f"const unsigned vui_assets_length = {len(data)};"]
target.write_text("\n".join(lines) + "\n")
