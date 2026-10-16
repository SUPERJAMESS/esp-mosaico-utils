from __future__ import annotations

import json
from typing import Any


def encode_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def decode_json(value: str | None, default: Any) -> Any:
    return default if value is None else json.loads(value)
