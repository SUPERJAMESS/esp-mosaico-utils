#!/usr/bin/env python3
"""Build a fixed, localized Ideas information card with embedded glyph subsets.

Run at publication time with the font covering this locale. No font engine or
complete multilingual font is downloaded to the device. Output is a deployable
GSPB, a bounded PNG list icon, and asset hashes for the cloud descriptor.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageFont


def wrap(text, font, width, max_lines):
    lines, line = [], ""
    for char in text:
        if char == "\n":
            lines.append(line)
            line = ""
        elif font.getlength(line + char) > width:
            split = line.rfind(" ")
            if split > 0:
                lines.append(line[:split])
                line = line[split + 1:] + char
            else:
                lines.append(line)
                line = char
        else:
            line += char
    lines.append(line)
    if len(lines) > max_lines:
        raise ValueError("Text does not fit the fixed presentation; shorten it before publishing")
    return "\n".join(lines)


def build(args):
    if len(args.code) != 6 or any(c not in "0123456789" for c in args.code):
        raise ValueError("code must contain exactly six ASCII digits")
    if not args.name or len(args.name.encode()) > 128 or len(args.description.encode()) > 512:
        raise ValueError("name/description exceeds the device UTF-8 byte limit")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    font_path = args.font.resolve()
    name = wrap(args.name, ImageFont.truetype(str(font_path), 28), 328, 2)
    description = wrap(args.description, ImageFont.truetype(str(font_path), 20), 416, 6)
    with Image.open(args.icon) as source:
        icon = source.convert("RGBA")
        icon.thumbnail((64, 64), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (64, 64))
        canvas.alpha_composite(icon, ((64-icon.width)//2, (64-icon.height)//2))
        canvas.save(output / "icon.png", optimize=True)

    def label(x, y, w, h, text, size, color="#FCFCFF"):
        return dict(type="label", parent=-1, x=x, y=y, w=w, h=h, text=text,
                    font_size=size, fg_color=color, text_align="left")

    # The reserved top-left 72x72 hit area is handled by Vibe, independently
    # of downloaded action IDs. This card cannot trigger device operations.
    scene = dict(screen="idea_info", w=480, h=480, screen_bg="#000000",
        font=str(font_path), default_font_size=20, swipe=False, objects=[
            dict(type="container", parent=-1, x=12, y=12, w=48, h=48, radius=12, bg_color="#242426"),
            label(22, 15, 30, 40, "<", 28),
            label(90, 20, 358, 35, args.code, 24, "#91919B"),
            dict(type="image", parent=-1, x=384, y=82, w=64, h=64, image="icon.png"),
            label(32, 82, 328, 72, name, 28),
            label(32, 176, 416, 170, description, 20, "#BDBDC6"),
        ])
    # Local firmware owns these controls and their hit areas. They stay hidden
    # in the offline information view; downloaded actions are never executed.
    controls = len(scene["objects"])
    scene["objects"].append(dict(type="layer", parent=-1, x=0, y=0, w=480, h=480,
        name="idea_download_controls", bind="download_controls", bind_target="visible", hidden=True))
    summary = label(24, 356, 432, 40, "", 16, "#BDBDC6")
    summary.update(parent=controls, name="idea_download_summary", bind="download_summary",
                   font_charset="".join(chr(i) for i in range(32, 127)))
    scene["objects"].append(summary)
    for x, caption in ((24, "Cancel"), (252, "Download")):
        scene["objects"].append(dict(type="container", parent=controls, x=x, y=408,
            w=204, h=56, radius=16, bg_color="#242426"))
        button = label(x, 408, 204, 56, caption, 20)
        button.update(parent=controls, text_align="center")
        if caption == "Download":
            button.update(name="idea_download_accept", bind="download_accept", font_charset="DownloadMy Ideas")
        scene["objects"].append(button)
    scene_path = output / "card.json"
    latin_font = Path(__file__).resolve().parents[1] / "ui/fonts/DejaVuSans.ttf"
    for index in (1, 2, controls + 1, controls + 3, controls + 5):
        scene["objects"][index]["font"] = str(latin_font)
    scene_path.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    profile = Path(__file__).resolve().parents[1] / "ui/profile.yaml"
    result = subprocess.run([str(args.gspc), "pack", str(scene_path), "--profile", str(profile),
                    "--pixel-format", "rgb565", "--deployable", "-o", str(output / "card.gspb")],
                    check=True, capture_output=True, text=True)
    print(result.stdout, end="")
    print(result.stderr, file=sys.stderr, end="")
    if "GSPC-RS-TEXT-MISSING" in result.stderr + result.stdout:
        raise ValueError("The supplied font does not cover the published text")
    assets = []
    for name, maximum in (("card.gspb", 1024*1024), ("icon.png", 65536)):
        data = (output / name).read_bytes()
        if len(data) > maximum:
            raise ValueError(name + " exceeds the device asset limit")
        assets.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    metadata = dict(code=args.code, name=args.name, description=args.description,
                    locale=args.locale, presentation="card.gspb", icon="icon.png", files=assets)
    (output / "presentation.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("code", "name", "description", "locale"):
        parser.add_argument("--" + name, required=True)
    for name in ("font", "icon", "output", "gspc"):
        parser.add_argument("--" + name, type=Path, required=True)
    build(parser.parse_args())
