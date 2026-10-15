#!/usr/bin/env python3
"""Reject stale authored resources without requiring graphics packages at build time."""

import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = root / "components/app_ui/light/vui_assets.sha256.json"
checks = json.loads(manifest.read_text())
changed = [
    name
    for name, digest in checks.items()
    if not (root / name).is_file()
    or hashlib.sha256((root / name).read_bytes()).hexdigest() != digest
]
if changed:
    raise SystemExit(
        "UI assets are stale; run tools/compile_vui.py after tools/build_vibe_ui.py.\n"
        + "\n".join(changed)
    )
