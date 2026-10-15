#!/usr/bin/env python3
"""Write Ideas display resources using the shared publication API."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

UTILS_ROOT = Path(__file__).resolve().parents[4]
if str(UTILS_ROOT) not in sys.path:
    sys.path.insert(0, str(UTILS_ROOT))
from shared.ideas import generate_resources  # noqa: E402


def build(args):
    payloads = generate_resources(name=args.name, description=args.description,
                                 locale=args.locale, font=args.font, icon=args.icon)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    assets = []
    for name, data in payloads.items():
        (output / name).write_bytes(data)
        assets.append(dict(path=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    return {"display": next(a for a in assets if a["path"] == "display.json"),
            "assets": [a for a in assets if a["path"] != "display.json"]}



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("name", "description", "locale"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--icon", type=Path)
    for name in ("font", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    print(json.dumps(build(parser.parse_args()), ensure_ascii=False, indent=2))
