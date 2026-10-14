"""Validate display/1 references, MIFF glyphs and their actual pixel bounds."""
from __future__ import annotations

from pathlib import PurePosixPath
import struct

from .ideas_content import MIB, image_check, path_valid, require, strict_json


def font_glyphs(raw, path):
    require(24 <= len(raw) <= MIB, "Invalid MIFF length", path, "INVALID_PRESENTATION")
    magic, version, header, length, count, pixels, line, flags, table = struct.unpack_from("<4sHHIHHHHI", raw)
    require(magic == b"MIFF" and version == 1 and header == table == 24 and length == len(raw)
            and 0 < count <= 512 and 0 < pixels <= 96 and 0 < line <= 192 and flags == 0,
            "Invalid MIFF header", path, "INVALID_PRESENTATION")
    end, previous, result = 24 + count*20, 0, {}
    require(end <= len(raw), "Truncated MIFF index", path, "INVALID_PRESENTATION")
    for i in range(count):
        gid, offset, w, h, left, top, advance = struct.unpack_from("<IIHHhhi", raw, 24+i*20)
        require(gid > previous and offset == end and w <= 192 and h <= 192
                and bool(w) == bool(h) and abs(left) <= 192 and abs(top) <= 192
                and abs(advance) <= 192*64 and end+w*h <= len(raw),
                "Invalid MIFF glyph", path, "INVALID_PRESENTATION")
        end += w*h
        previous = gid
        result[gid] = w, h, left, top
    require(end == len(raw), "Trailing MIFF bytes", path, "INVALID_PRESENTATION")
    return result


def validate_display(presentation, files):
    path = presentation["display"]["path"]
    directory = str(PurePosixPath(path).parent)
    locale = directory.rsplit("/", 1)[-1]
    document = strict_json(files[path], 32768, 8, path)
    require(set(document) == {"schema", "locale", "name", "description", "icon"}
            and document["schema"] == "mosaico-ideas/display/1" and document["locale"] == locale,
            "Invalid display fields or locale", path, "INVALID_PRESENTATION")
    assets = {ref["path"] for ref in presentation["assets"]}
    require(all(str(PurePosixPath(p).parent) == directory for p in assets),
            "Display assets must be in the same language directory", path, "INVALID_PRESENTATION")
    require(len(files[path]) + sum(len(files[p]) for p in assets) <= MIB,
            "Language display exceeds 1 MiB", path, "INVALID_PRESENTATION")
    used, fonts = set(), {}
    def resolve(name):
        require(path_valid(name) and "/" not in name and name.lower() != "display.json",
                "Display reference must be a resource basename", path, "INVALID_PRESENTATION")
        target = directory + "/" + name
        require(target in assets, "Missing display resource", target, "INVALID_PRESENTATION")
        used.add(target)
        return target
    icon = document["icon"]
    require(isinstance(icon, str), "Invalid icon reference", path, "INVALID_PRESENTATION")
    if icon:
        image_check(files[resolve(icon)], directory + "/" + icon, icon=True)
    for field, max_w, max_h, max_text in (("name", 328, 84, 128), ("description", 416, 176, 512)):
        block = document[field]
        require(isinstance(block, dict) and set(block) == {"text", "font", "width", "height", "glyphs"},
                "Invalid text block fields", path, "INVALID_PRESENTATION")
        text = block["text"]
        require(isinstance(text, str) and len(text.encode("utf-8")) <= max_text
                and (bool(text) or field == "description")
                and all(ord(c) >= 32 or c == "\n" for c in text),
                "Invalid display text", path, "INVALID_PRESENTATION")
        w, h, runs = block["width"], block["height"], block["glyphs"]
        require(type(w) is int and type(h) is int and 0 < w <= max_w and 0 < h <= max_h
                and isinstance(runs, list) and len(runs) <= 512 and (runs or not text),
                "Invalid display dimensions or glyph count", path, "INVALID_PRESENTATION")
        font = resolve(block["font"])
        require(font != directory + "/" + icon, "Icon reused as font", path, "INVALID_PRESENTATION")
        if font not in fonts:
            fonts[font] = font_glyphs(files[font], font)
        for run in runs:
            require(isinstance(run, list) and len(run) == 3 and all(type(v) is int for v in run),
                    "Invalid glyph run", path, "INVALID_PRESENTATION")
            gid, x, y = run
            require(gid in fonts[font] and -192 <= x <= w+192 and -192 <= y <= h+192,
                    "Unknown glyph or invalid position", path, "INVALID_PRESENTATION")
            gw, gh, left, top = fonts[font][gid]
            require(not gw or (0 <= x+left <= w-gw and 0 <= y-top <= h-gh),
                    "Glyph pixels leave the text block", path, "INVALID_PRESENTATION")
    require(1 <= len(fonts) <= 2 and used == assets, "Unreferenced display assets", path, "INVALID_PRESENTATION")
    return document
