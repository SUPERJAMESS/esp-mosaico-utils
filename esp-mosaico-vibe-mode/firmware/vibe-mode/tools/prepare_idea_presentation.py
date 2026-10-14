#!/usr/bin/env python3
"""Publish UI-independent fixed text: display/1 JSON, MIFF/1 glyphs and PNG.

Install requirements-presentation.txt and the host's libraqm (>= 0.9).
The supplied font must cover the selected locale. Only the glyphs used by the
fixed name/description are distributed; font licensing remains the publisher's
responsibility. No GSP compiler, scenes or widget definitions are involved.
"""
import argparse
import hashlib
import json
import io
from pathlib import Path

from PIL import Image
from ideas_font import Font


def resources(args):
    """Generate identity-free files before the release manifest is frozen."""
    if not args.name or len(args.name.encode()) > 128 or len(args.description.encode()) > 512:
        raise ValueError("name/description exceeds the device UTF-8 byte limit")
    if not args.locale or len(args.locale.encode()) > 23 or not args.locale.isascii():
        raise ValueError("locale must be a short ASCII language tag")
    if any(ord(c) < 32 and c != "\n" for c in args.name + args.description):
        raise ValueError("Only newline controls are supported in published text")
    title_font = Font(args.font.resolve(), 28, args.locale)
    body_font = Font(args.font.resolve(), 20, args.locale)
    title = title_font.layout(args.name, 328, 84)
    body = body_font.layout(args.description, 416, 176)
    title["font"], body["font"] = "title.mif", "body.mif"
    display = dict(schema="mosaico-ideas/display/1", locale=args.locale,
                   icon="icon.png" if args.icon else "", name=title, description=body)
    payloads = {"display.json": (json.dumps(display, ensure_ascii=False, separators=(",", ":")) + "\n").encode(),
                "title.mif": title_font.encode(), "body.mif": body_font.encode()}
    if args.icon:
        if not 0 < args.icon.stat().st_size <= 10*1024*1024:
            raise ValueError("Icon source exceeds 10 MiB")
        with Image.open(args.icon) as source:
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


def build(args):
    payloads = resources(args)
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
