#!/usr/bin/env python3
"""Compile Vibe's authored layout and original font/icon assets without a UI framework.

Authoring only: pip install Pillow zopfli. Normal device/PC builds embed the
checked-in, losslessly compressed output; they do not need graphics packages.
"""

import hashlib
import json
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import zopfli.zlib
from PIL import Image, ImageDraw, ImageFont, ImageSequence

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "ui"
OUT = ROOT / "components/app_ui/light"
TYPES = [
    "layer",
    "container",
    "label",
    "button",
    "image",
    "list",
    "progress",
    "keyboard",
    "dots",
    "matrix",
]
NODE = "<5h15HI"


def color(s):
    c = int(s.lstrip("#")[:6], 16)
    return ((c >> 19) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31)


def svg(path):
    tree = ET.parse(path).getroot()
    scale = 4
    im = Image.new(
        "RGBA", (int(tree.attrib["width"]) * scale, int(tree.attrib["height"]) * scale)
    )
    d = ImageDraw.Draw(im)

    def draw(el, fill="#000000"):
        fill = el.get("fill", fill)
        if el.tag.endswith("rect"):
            x, y, w, h = [
                float(el.get(k, 0)) * scale for k in ["x", "y", "width", "height"]
            ]
            d.rounded_rectangle(
                (x, y, x + w - 1, y + h - 1),
                radius=float(el.get("rx", 0)) * scale,
                fill=fill,
            )
        for child in el:
            draw(child, fill)

    draw(tree)
    return im.resize((im.width // scale, im.height // scale), Image.Resampling.LANCZOS)


def compile_assets():
    scene = json.loads((UI / "main.json").read_text())
    objects = scene["objects"]
    # Expand the two fixed keyboards into ordinary nodes. No widget framework,
    # runtime layout interpreter, or embedded font rasterizer is involved.
    for i, o in list(enumerate(objects)):
        if o["type"] != "keyboard":
            continue
        name = o["name"]
        for mode, rows in enumerate(
            [
                ("qwertyuiop", "asdfghjkl", "zxcvbnm"),
                ("QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"),
                ("1234567890", "@#$%&*-+=", "!?.,:/;"),
                ("1234567890", "_\\|~^[]{}", "<>*!+=-"),
            ]
        ):
            parent = len(objects)
            objects.append(
                {
                    "type": "layer",
                    "parent": i,
                    "x": 0,
                    "y": 0,
                    "w": 480,
                    "h": 233,
                    "name": name + ["_lower", "_upper", "_sym", "_sym2"][mode],
                    "hidden": mode != 0,
                    "key_mode": True,
                }
            )

            def key(x, y, w, text, arg, parent=parent, name=name):
                objects.append(
                    {
                        "type": "button",
                        "parent": parent,
                        "x": x,
                        "y": y,
                        "w": w,
                        "h": 52,
                        "text": text,
                        "font": "fonts/VibeMedium.otf",
                        "font_size": 22 if len(text) == 1 else 18,
                        "font_charset": "".join(chr(c) for c in range(32, 127)),
                        "bg_color": "#F3F4F2" if arg == 13 else "#272A2D",
                        "fg_color": "#000000" if arg == 13 else "#ECECF0",
                        "radius": 8,
                        "events": [{"target_name": name + "_key", "arg": arg}],
                    }
                )

            for r, row in enumerate(rows):
                x0 = [12, 35, 81][r]
                for c, ch in enumerate(row):
                    key(x0 + c * 46, r * 59, 42, ch, ord(ch))
            for x, icon, arg in [
                (12, o["shift_icon"], 0x103 if mode == 2 else 0x102 if mode == 3 else 0x100 if mode != 1 else 0x101),
                (408, o["delete_icon"], 8),
            ]:
                key(x, 118, 60, "#+=" if x == 12 and mode == 2 else "123" if x == 12 and mode == 3 else "", arg)
                if x != 12 or mode < 2: objects.append(
                    {
                        "type": "image",
                        "parent": parent,
                        "x": x + 14,
                        "y": 128,
                        "w": 32,
                        "h": 32,
                        "image": icon,
                    }
                )
            key(
                12,
                177,
                76,
                "ABC" if mode >= 2 else "123",
                0x101 if mode >= 2 else 0x102,
            )
            key(96, 177, 288, "space", 32)
            objects[-1]["name"] = name + "_space_" + str(mode)
            key(392, 177, 76, "GO", 13)
    actions = {"none": 0}
    for o in objects:
        for e in o.get("events", []):
            actions.setdefault(e["target_name"], len(actions))
        if o.get("callback"):
            actions.setdefault(o["callback"], len(actions))
    fontchars = {}

    def font_key(o):
        return (
            o.get("font", scene["font"]),
            o.get("font_size", scene.get("font_size", 20)),
        )

    for o in objects:
        if "text" in o:
            fontchars.setdefault(font_key(o), set()).update(
                o.get("font_charset", "") + o["text"]
            )
    fonts = list(fontchars)
    images = list(dict.fromkeys(o["image"] for o in objects if o.get("image")))
    blob = bytearray(
        32 + len(objects) * struct.calcsize(NODE) + len(fonts) * 16 + len(images) * 12
    )

    def append(data):
        blob.extend(b"\0" * (-len(blob) % 4))
        pos = len(blob)
        blob.extend(data)
        return pos

    strings = {}

    def string(text):
        if text not in strings:
            strings[text] = append(text.encode() + b"\0")
        return strings[text]

    templates = {o["template"]: i for i, o in enumerate(objects) if o.get("template")}
    ids = {}
    for i, o in enumerate(objects):
        if o.get("bind"):
            ids["VUI_VIBE_BIND_" + o["bind"].upper()] = i + 1
        if o.get("name"):
            ids["VUI_VIBE_OBJ_KEY_" + o["name"].upper()] = i + 1
            ids.setdefault("VUI_VIBE_BIND_" + o["name"].upper() + "_VISIBLE", i + 1)
        flags = (
            (1 if o.get("hidden") else 0)
            | (2 if o.get("template") else 0)
            | (4 if o.get("bg_color") and not o["bg_color"].endswith("000001") else 0)
            | (8 if o.get("overflow") == "ellipsis" else 0)
            | (16 if o.get("key_mode") else 0)
            | (32 if o.get("scroll") else 0)
            | (64 if o.get("press_ring") else 0)
            | (128 if o.get("overflow") == "marquee" else 0)
        )
        e = o.get("events", [{}])[0]
        values = [
            o.get("parent", -1),
            o.get("x", 0),
            o.get("y", 0),
            o.get("w", 0),
            o.get("h", 0),
            TYPES.index(o["type"]),
            flags,
            o.get("radius", 0),
            color(o.get("bg_color", "#000000")),
            color(o.get("fg_color", "#F2F2F2")),
            color(o.get("border_color", "#000000")),
            o.get("border_width", 0),
            fonts.index(font_key(o)) if "text" in o else 0,
            ["left", "center", "right"].index(
                o.get("text_align", "center" if o["type"] == "button" else "left")
            ),
            actions.get(e.get("target_name", o.get("callback")), 0),
            int(e.get("arg", 0)),
            images.index(o["image"]) + 1 if o.get("image") else 0,
            templates.get(o.get("row_template"), -1) + 1,
            o.get("item_height", 0),
            o.get("content_height", 0),
            string(o.get("text", "")),
        ]
        struct.pack_into(NODE, blob, 32 + i * struct.calcsize(NODE), *values)
    for i, (path, size) in enumerate(fonts):
        font = ImageFont.truetype(
            str(UI / path), size, layout_engine=ImageFont.Layout.BASIC
        )
        glyphs = []
        for ch in sorted(fontchars[(path, size)] - {"\n", "\r"}, key=ord):
            x, y, r, b = font.getbbox(ch, anchor="ls")
            w, h = r - x, b - y
            im = Image.new("L", (max(1, w), max(1, h)))
            ImageDraw.Draw(im).text((-x, -y), ch, font=font, fill=255, anchor="ls")
            if not ch.isspace() and not im.getbbox():
                raise ValueError(f'{path} ({size}px): empty glyph {ch!r} U+{ord(ch):04X}')
            glyphs.append(
                struct.pack(
                    "<II6h",
                    ord(ch),
                    append(im.tobytes() if w * h else b""),
                    x,
                    y,
                    w,
                    h,
                    round(font.getlength(ch) * 64),
                    0,
                )
            )
        offset = append(b"".join(glyphs))
        ascent, descent = font.getmetrics()
        struct.pack_into(
            "<II4h",
            blob,
            32 + len(objects) * struct.calcsize(NODE) + i * 16,
            offset,
            len(glyphs),
            ascent,
            descent,
            ascent + descent,
            0,
        )
    for i, path in enumerate(images):
        im = svg(UI / path) if path.endswith(".svg") else Image.open(UI / path)
        source_frames = ImageSequence.Iterator(im)
        duration = im.info.get("duration", 100)
        frames = []
        for f in source_frames:
            data = bytearray()
            for r, g, b, a in f.convert("RGBA").getdata():
                data += struct.pack(
                    "<HB", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), a
                )
            frames.append(data)
        offset = append(b"".join(frames))
        struct.pack_into(
            "<I4H",
            blob,
            32 + len(objects) * struct.calcsize(NODE) + len(fonts) * 16 + i * 12,
            offset,
            im.width,
            im.height,
            len(frames),
            max(20, duration),
        )
    struct.pack_into(
        "<8I",
        blob,
        0,
        0x31554956,
        len(blob),
        32,
        len(objects),
        32 + len(objects) * struct.calcsize(NODE),
        len(fonts),
        32 + len(objects) * struct.calcsize(NODE) + len(fonts) * 16,
        len(images),
    )
    packed = zopfli.zlib.compress(bytes(blob), numiterations=15)
    (OUT / "vui_assets.bin").write_bytes(struct.pack("<I", len(blob)) + packed)
    lines = [
        "// Generated by tools/compile_vui.py; do not edit.",
        "#pragma once",
        '#include "vui.h"',
    ]
    lines += [f"#define {k} {v}" for k, v in ids.items()]
    lines += [f"#define VUI_VIBE_ACT_ID_{k.upper()} {v}" for k, v in actions.items()]
    for template, bind in [
        ("networks", "title"),
        ("networks", "detail"),
        ("bundles", "title"),
        ("bundles", "detail"),
    ]:
        lines.append(
            f"static inline vui_err_t vui_vibe_{template}_row_row_set_{template}_{bind}_text(vui_handle_t ui, vui_row_t row, const char *s) {{ return vui_row_set_text(ui,row,VUI_VIBE_BIND_{template.upper()}_{bind.upper()},s); }}"
        )
    for suffix in ["DETAIL_FG_COLOR", "ICON_RESOURCE"]:
        lines.append(
            f"#define VUI_VIBE_TEMPLATE_BUNDLES_ROW_BUNDLES_{suffix}_SLOT VUI_VIBE_BIND_BUNDLES_{suffix.split('_')[0]}"
        )
    (OUT.parent / "private/vui_ids.h").write_text("\n".join(lines) + "\n")
    inputs = (
        ["tools/compile_vui.py", "tools/build_vibe_ui.py", "tools/vibe_design.py",
         "ui/core.json", "ui/main.json", "ui/translations.json"]
        + ["ui/" + p for p, _ in fonts]
        + ["ui/" + p for p in images]
    )
    inputs = list(dict.fromkeys(inputs))
    outputs = [
        "components/app_ui/light/vui_assets.bin",
        "components/app_ui/private/vui_ids.h",
    ]
    checks = {
        p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in inputs + outputs
    }
    (OUT / "vui_assets.sha256.json").write_text(
        json.dumps(checks, indent=2, sort_keys=True) + "\n"
    )
    print(
        f"{len(objects)} nodes, {len(fonts)} fonts, {len(images)} images; {len(blob)} raw / {len(packed) + 4} compressed bytes"
    )


if __name__ == "__main__":
    compile_assets()
