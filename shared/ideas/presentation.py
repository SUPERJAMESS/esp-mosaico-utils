"""Generate identity-free Ideas display resources for publication hosts."""
from __future__ import annotations

import io
import json
from pathlib import Path
from typing import Dict, Optional

from PIL import Image

from .font import Font


def generate_resources(*, name: str, description: str, locale: str,
                       font: Path, icon: Optional[Path] = None) -> Dict[str, bytes]:
    """Return display/1, MIFF/1 and optional PNG bytes without writing files.
    Raise ValueError for invalid/device-incompatible input; propagate I/O errors.
    """
    if not name or len(name.encode()) > 128 or len(description.encode()) > 512:
        raise ValueError("name/description exceeds the device UTF-8 byte limit")
    if not locale or len(locale.encode()) > 23 or not locale.isascii():
        raise ValueError("locale must be a short ASCII language tag")
    if any(ord(c) < 32 and c != "\n" for c in name + description):
        raise ValueError("Only newline controls are supported in published text")
    title_font = Font(font.resolve(), 28, locale)
    body_font = Font(font.resolve(), 20, locale)
    title = title_font.layout(name, 328, 84)
    body = body_font.layout(description, 416, 176)
    title["font"], body["font"] = "title.mif", "body.mif"
    display = dict(schema="mosaico-ideas/display/1", locale=locale,
                   icon="icon.png" if icon else "", name=title, description=body)
    payloads = {"display.json": (json.dumps(display, ensure_ascii=False, separators=(",", ":")) + "\n").encode(),
                "title.mif": title_font.encode(), "body.mif": body_font.encode()}
    if icon:
        if not 0 < icon.stat().st_size <= 10*1024*1024:
            raise ValueError("Icon source exceeds 10 MiB")
        with Image.open(icon) as source:
            if (source.format not in ("JPEG", "PNG", "GIF", "WEBP") or
                    not 0 < source.width <= 4096 or not 0 < source.height <= 4096 or
                    getattr(source, "n_frames", 1) != 1 or getattr(source, "is_animated", False)):
                raise ValueError("Icon source must be a static image no larger than 4096 pixels per axis")
            icon = source.convert("RGBA")
            icon.thumbnail((64, 64), Image.Resampling.LANCZOS)
            canvas = Image.new("RGBA", (64, 64))
            canvas.alpha_composite(icon, ((64-icon.width)//2, (64-icon.height)//2))
            output = io.BytesIO()
            canvas.save(output, format="PNG", optimize=True)
            payloads["icon.png"] = output.getvalue()
    if len(payloads["display.json"]) > 32768 or len(payloads.get("icon.png", b"")) > 65536 or sum(map(len, payloads.values())) > 1024*1024:
        raise ValueError("Presentation exceeds the device asset limits")
    return payloads
