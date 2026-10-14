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
from pathlib import Path

from PIL import Image
from ideas_font import Font


def build(args):
    if len(args.code) != 6 or any(c not in "0123456789" for c in args.code):
        raise ValueError("code must contain exactly six ASCII digits")
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
                   icon="icon.png", name=title, description=body)
    payloads = {"display.json": (json.dumps(display, ensure_ascii=False, separators=(",", ":")) + "\n").encode(),
                "title.mif": title_font.encode(), "body.mif": body_font.encode()}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with Image.open(args.icon) as source:
        icon = source.convert("RGBA")
        icon.thumbnail((64, 64), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (64, 64))
        canvas.alpha_composite(icon, ((64-icon.width)//2, (64-icon.height)//2))
        canvas.save(output / "icon.png", optimize=True)
    payloads["icon.png"] = (output / "icon.png").read_bytes()
    if len(payloads["display.json"]) > 32768 or len(payloads["icon.png"]) > 65536 or sum(map(len, payloads.values())) > 1024*1024:
        raise ValueError("Presentation exceeds the device asset limits")
    assets = []
    for name, data in payloads.items():
        (output / name).write_bytes(data)
        assets.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    metadata = dict(schema="mosaico-ideas/presentation-assets/1", download_schema="mosaico-ideas/download/1",
                    code=args.code, name=args.name, description=args.description,
                    locale=args.locale, presentation="display.json", icon="icon.png",
                    fonts=["title.mif", "body.mif"], files=assets)
    (output / "presentation.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("code", "name", "description", "locale"):
        parser.add_argument("--" + name, required=True)
    for name in ("font", "icon", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    build(parser.parse_args())
