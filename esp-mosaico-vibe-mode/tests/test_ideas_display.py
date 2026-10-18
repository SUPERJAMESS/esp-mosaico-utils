"""Compile the portable readers with no GSP/ESP headers; roundtrip published text."""
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from types import SimpleNamespace

import pytest

RECOVERY = Path(__file__).resolve().parents[1] / "firmware/vibe-mode"


class Font(C.Structure):
    _fields_ = [("data", C.c_void_p), ("size", C.c_size_t), ("count", C.c_uint16),
                ("pixels", C.c_uint16), ("line_height", C.c_uint16)]


class Source(C.Structure):
    _fields_ = [("name", C.c_char_p), ("data", C.c_char_p), ("size", C.c_size_t)]


class Sources(C.Structure):
    _fields_ = [(key, C.c_char_p) for key in ("name", "description", "locale", "icon")] + [
        ("font_count", C.c_size_t), ("fonts", Source * 2)]


class Position(C.Structure):
    _fields_ = [("glyph", C.c_uint32), ("x", C.c_int16), ("y", C.c_int16)]


class Text(C.Structure):
    _fields_ = [(key, C.c_uint16) for key in ("width", "height", "count", "font")] + [
        ("glyphs", Position * 512)]


class Display(C.Structure):
    _fields_ = [("fonts", Font * 2), ("text", Text * 2)]


@pytest.fixture(scope="module")
def core(tmp_path_factory):
    directory = tmp_path_factory.mktemp("display-reader")
    dependency = Path(os.environ.get("CJSON_COMPONENT_DIR", str(RECOVERY / "managed_components/espressif__cjson")))
    cjson = next(dependency.rglob("cJSON.c"))
    output = directory / "reader.so"
    # cJSON explicitly exports its Windows API, disabling MinGW's default
    # automatic exports for the readers linked into the same test library.
    exports = ["-Wl,--export-all-symbols"] if os.name == "nt" else []
    subprocess.run([shutil.which("cc"), "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
        "-I" + str(cjson.parent), "-I" + str(RECOVERY / "components/app_ideas/include"), "-I" + str(RECOVERY / "components/app_ideas/private"), str(RECOVERY / "components/app_ideas/src/ideas_font.c"),
        str(RECOVERY / "components/app_ideas/src/ideas_presentation.c"), str(cjson), "-o", str(output)] + exports, check=True)
    lib = C.CDLL(str(output))
    lib.ideas_font_open.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Font)]
    lib.ideas_font_open.restype = C.c_bool
    lib.ideas_presentation_parse.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Sources), C.POINTER(Display)]
    lib.ideas_presentation_parse.restype = C.c_bool
    lib.ideas_presentation_render.argtypes = [C.POINTER(Display), C.c_uint, C.c_uint16, C.c_uint16,
                                              C.POINTER(C.c_uint16), C.c_size_t]
    lib.ideas_presentation_render.restype = C.c_bool
    return lib


def font():
    return struct.pack("<4sHHIHHHHI", b"MIFF", 1, 24, 50, 1, 20, 24, 0, 24) + \
        struct.pack("<IIHHhhi", 42, 44, 2, 3, 0, 3, 128) + bytes([255, 128, 0, 255, 255, 0])


def document():
    return dict(schema="mosaico-ideas/display/1", locale="en", icon="icon.png",
        name=dict(text="A", font="font.mif", width=328, height=84, glyphs=[[42, 0, 3]]),
        description=dict(text="B", font="font.mif", width=416, height=176, glyphs=[[42, 4, 5]]))


def parse(core, data, blob=None):
    blob = font() if blob is None else blob
    raw = json.dumps(data).encode() if isinstance(data, dict) else data
    sources = Sources(b"A", b"B", b"en", b"icon.png", 1, (Source * 2)(Source(b"font.mif", blob, len(blob))))
    display = Display()
    ok = core.ideas_presentation_parse(raw, len(raw), C.byref(sources), C.byref(display))
    return ok, display, sources  # sources pins borrowed font bytes


def test_portable_render_and_alpha_blending(core):
    ok, display, sources = parse(core, document())
    assert ok
    pixels = (C.c_uint16 * (328 * 84))()
    assert core.ideas_presentation_render(C.byref(display), 0, 0xffff, 0, pixels, len(pixels))
    assert pixels[0] == 0xffff and pixels[1] not in (0, 0xffff)
    assert pixels[328] == 0 and pixels[329] == 0xffff
    assert sum(p != 0 for p in pixels) == 4
    assert not core.ideas_presentation_render(C.byref(display), 0, 0xffff, 0, pixels, len(pixels)-1)


@pytest.mark.parametrize("offset,fmt,value", [
    (0,"4s",b"GSPB"), (4,"H",2), (6,"H",23), (8,"I",51), (12,"H",513),
    (14,"H",0), (16,"H",193), (18,"H",1), (20,"I",25), (24,"I",0),
    (28,"I",43), (28,"I",45), (32,"H",193), (34,"H",0), (36,"h",193), (40,"i",12300),
])
def test_rejects_malformed_font(core, offset, fmt, value):
    blob = bytearray(font()); struct.pack_into("<" + fmt, blob, offset, value)
    assert not core.ideas_font_open(bytes(blob), len(blob), C.byref(Font()))


def test_every_truncation_and_trailing_byte_rejected(core):
    blob = font()
    for size in range(len(blob)):
        assert not core.ideas_font_open(blob[:size], size, C.byref(Font()))
    assert not core.ideas_font_open(blob + b"x", len(blob) + 1, C.byref(Font()))


@pytest.mark.parametrize("change", [
    lambda d: d.update(schema="mosaico-ideas/display/2"),
    lambda d: d.update(locale="ar"), lambda d: d.update(icon="../../icon.png"),
    lambda d: d.update(actions=["install"]), lambda d: d["name"].update(text="different app"),
    lambda d: d["name"].update(font="https://example.test/font.mif"),
    lambda d: d["name"].update(font="display.json"),
    lambda d: d["name"].update(width=329), lambda d: d["name"].update(height=85),
    lambda d: d["name"].update(glyphs=[[43, 0, 3]]),
    lambda d: d["name"].update(glyphs=[[42, -1, 3]]),
    lambda d: d["name"].update(glyphs=[[42, 327, 3]]),
    lambda d: d["name"].update(glyphs=[[42, 0, 2]]),
    lambda d: d["name"].update(glyphs=[[42, 0, 85]]),
    lambda d: d["name"].update(glyphs=[[42, 0.5, 3]]),
    lambda d: d["name"].update(glyphs=[[42, 0, 3]]*513),
    lambda d: d["name"].update(glyphs=[]),
])
def test_rejects_bad_display_or_dependency(core, change):
    data = document(); change(data)
    assert not parse(core, data)[0]


def test_duplicate_keys_nul_depth_and_trailing_data(core):
    raw = json.dumps(document()).encode()
    for bad in [raw.replace(b'"schema":', b'"icon":"icon.png","schema":'),
                raw.replace(b'"A"', b'"A\\u0000hidden"'), raw + b"{}", b"[" * 1000 + b"]" * 1000]:
        assert not parse(core, bad)[0]


@pytest.fixture(scope="module")
def publisher():
    spec = importlib.util.spec_from_file_location("prepare_idea_presentation", RECOVERY / "tools/prepare_idea_presentation.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("locale,name,description,font_path", [
    ("en", "Weather", "Clear skies today.\nTomorrow: 24 C.", RECOVERY / "ui/fonts/DejaVuSans.ttf"),
    ("ar", "مرحبا بالعالم", "وصف التطبيق باللغة العربية", RECOVERY / "ui/fonts/DejaVuSans.ttf"),
    ("zh-CN", "天气预报", "查看今天的天气。", Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")),
])
def test_publisher_roundtrip_without_ui_framework(core, publisher, tmp_path, locale, name, description, font_path):
    if not font_path.exists(): pytest.skip("Publication font unavailable")
    from PIL import Image
    icon = tmp_path / "source.png"; Image.new("RGB", (128, 80), "red").save(icon)
    metadata = publisher.build(SimpleNamespace(name=name, description=description,
        locale=locale, font=font_path, icon=icon, output=tmp_path / "out"))
    assert not list((tmp_path / "out").glob("*.gspb"))
    assert not (tmp_path / "out/presentation.json").exists()
    for asset in [metadata["display"]] + metadata["assets"]:
        raw = (tmp_path / "out" / asset["path"]).read_bytes()
        assert len(raw) == asset["size"] and hashlib.sha256(raw).hexdigest() == asset["sha256"]
    names = ["title.mif", "body.mif"]
    blobs = [(tmp_path / "out" / n).read_bytes() for n in names]
    fonts = (Source * 2)(*[Source(n.encode(), raw, len(raw)) for n,raw in zip(names, blobs)])
    sources = Sources(name.encode(), description.encode(), locale.encode(), b"icon.png", 2, fonts)
    raw = (tmp_path / "out/display.json").read_bytes(); display = Display()
    assert core.ideas_presentation_parse(raw, len(raw), C.byref(sources), C.byref(display))
    pixels = (C.c_uint16 * (328*84))()
    assert core.ideas_presentation_render(C.byref(display), 0, 0xffff, 0, pixels, len(pixels))
    assert sum(p != 0 for p in pixels) > 50
    if locale == "ar":
        import freetype
        face = freetype.Face(str(font_path))
        shaped = {display.text[0].glyphs[i].glyph for i in range(display.text[0].count)}
        assert shaped - {face.get_char_index(ord(c)) for c in name}, "Arabic contextual glyphs must survive serialization"


def test_missing_glyphs_and_overflow_fail_publication(publisher):
    from shared.ideas.font import Font as PublicationFont
    font = PublicationFont(RECOVERY / "ui/fonts/DejaVuSans.ttf", 28, "en")
    with pytest.raises(ValueError, match="cover"):
        font.layout("中文", 328, 84)
    with pytest.raises(ValueError, match="fit"):
        font.layout("word " * 100, 328, 84)


def test_empty_description_and_newline_only_text(publisher, core):
    from shared.ideas.font import Font as PublicationFont
    font = PublicationFont(RECOVERY / "ui/fonts/DejaVuSans.ttf", 20, "en")
    assert font.layout("", 416, 176)["glyphs"] == []
    assert core.ideas_font_open(font.encode(), len(font.encode()), C.byref(Font()))
    with pytest.raises(ValueError, match="Nonempty"):
        font.layout("\n", 416, 176)
