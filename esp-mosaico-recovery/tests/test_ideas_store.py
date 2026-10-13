"""Exercise the actual bounded parser and NAND transaction using host files."""
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import pytest

RECOVERY = Path(__file__).resolve().parents[1] / "firmware/recovery"


class File(C.Structure):
    _fields_ = [("name", C.c_char * 65), ("path", C.c_char * 193),
                ("sha256", C.c_char * 65), ("size", C.c_uint32)]


class Package(C.Structure):
    _fields_ = [("code", C.c_char * 7)] + [
        (key, C.c_char * 65) for key in ("application_id", "release_id", "version")
    ] + [("name", C.c_char * 129), ("description", C.c_char * 513),
         ("locale", C.c_char * 24)] + [
        (key, C.c_char * 65) for key in ("presentation", "icon", "application_sha256", "layout_sha256")
    ] + [(key, C.c_uint32) for key in ("application_size", "application_offset", "flash_size")] + [
        ("file_count", C.c_size_t), ("component_count", C.c_size_t),
        ("total_size", C.c_uint64), ("files", File * 12), ("manifest", C.c_char * 3073),
        ("font_count", C.c_size_t), ("fonts", (C.c_char * 65) * 2)]


CALLBACK = C.CFUNCTYPE(C.c_int, C.POINTER(File), C.c_char_p, C.c_void_p)


@pytest.fixture(scope="module")
def core(tmp_path_factory):
    build = tmp_path_factory.mktemp("ideas-core")
    (build / "esp_err.h").write_text("""typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
""")
    cjson = next((RECOVERY / "managed_components/espressif__cjson").rglob("cJSON.c"))
    result = subprocess.run([shutil.which("cc"), "-shared", "-fPIC", "-std=gnu11",
        "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-I" + str(build),
        "-I" + str(cjson.parent), str(RECOVERY / "main/ideas_package.c"),
        str(RECOVERY / "main/ideas_store.c"), str(cjson), "-o", str(build / "core.so")],
        text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    lib = C.CDLL(str(build / "core.so"))
    lib.ideas_package_parse.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_download.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t, C.POINTER(Package), CALLBACK, CALLBACK, C.c_void_p]
    lib.ideas_store_read.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_verify.argtypes = [C.c_char_p, C.POINTER(Package), CALLBACK, C.c_void_p]
    lib.ideas_store_recover.argtypes = [C.c_char_p, C.c_char_p]
    return lib


def descriptor(version="1.0", code="042731"):
    payloads = {"table.bin": b"t" * 4096, "app.bin": version.encode() * 200,
                "display.json": b"description", "title.mif": b"title glyphs",
                "body.mif": b"body glyphs", "icon.png": b"icon"}
    files = [dict(name=name, path="/api/v1/ideas/files/" + name, size=len(data),
                  sha256=hashlib.sha256(data).hexdigest()) for name, data in payloads.items()]
    components = [dict(id=i+1, kind=kind, target_offset=offset, file=f["name"],
                       size=f["size"], sha256=f["sha256"])
                  for i, (f, kind, offset) in enumerate(zip(files, ("partition_table", "application"), (0x8000, 0x210000)))]
    return dict(schema="mosaico-ideas/download/1", application=dict(code=code, id="stable-app-id",
        release_id="release-" + version, version=version, name="天气", description="天气预报", locale="zh-CN"),
        system_manifest=dict(schema="esp-iris-system-update/0.2", release=version,
            target=dict(chip_id=32, flash_size=16*1024*1024),
            target_layout_sha256=files[0]["sha256"], components=components),
        files=files, presentation="display.json", fonts=["title.mif", "body.mif"], icon="icon.png"), payloads


def parse(core, document, code=b"042731"):
    raw = document if isinstance(document, bytes) else json.dumps(document, ensure_ascii=False).encode()
    package = Package()
    return core.ideas_package_parse(raw, len(raw), code, C.byref(package)), package, raw


def callbacks(payloads, fail=None, corrupt=False):
    @CALLBACK
    def fetch(file, path, _):
        name = file.contents.name.decode()
        if name == fail:
            Path(path.decode()).write_bytes(b"partial")
            return -1
        Path(path.decode()).write_bytes(payloads[name] + (b"bad" if corrupt else b""))
        return 0

    @CALLBACK
    def verify(file, path, _):
        data = Path(path.decode()).read_bytes()
        return 0 if len(data) == file.contents.size and hashlib.sha256(data).hexdigest().encode() == file.contents.sha256 else -1
    return fetch, verify


def test_single_version_replacement_and_offline_assets(core, tmp_path):
    root = str(tmp_path / "ideas").encode()
    for version in ("1.0", "2.0"):
        document, payloads = descriptor(version)
        result, package, raw = parse(core, document)
        assert result == 0
        assert package.code == b"042731" and package.name.decode() == "天气"
        fetch, verify = callbacks(payloads)
        assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) == 0
        assert core.ideas_store_verify(root, C.byref(package), verify, None) == 0
        current = tmp_path / "ideas/042731/current"
        assert set(p.name for p in current.iterdir()) == set(payloads) | {"manifest.json", "idea.json"}
        assert sorted(p.name for p in current.parent.iterdir()) == ["current"]
        assert json.loads((current / "idea.json").read_text())["application"]["version"] == version


@pytest.mark.parametrize("failure", ["network", "hash"])
def test_failed_update_keeps_previous_download(core, tmp_path, failure):
    root = str(tmp_path).encode()
    document, payloads = descriptor()
    _, package, raw = parse(core, document)
    fetch, verify = callbacks(payloads)
    assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) == 0
    document, payloads = descriptor("2.0")
    _, package, raw = parse(core, document)
    fetch, verify = callbacks(payloads, fail="app.bin" if failure == "network" else None, corrupt=failure == "hash")
    assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) != 0
    saved = Package()
    assert core.ideas_store_read(root, b"042731", C.byref(saved)) == 0
    assert saved.version == b"1.0"
    assert sorted(p.name for p in (tmp_path / "042731").iterdir()) == ["current"]


def test_power_loss_between_generation_renames(core, tmp_path):
    root = str(tmp_path).encode()
    document, payloads = descriptor()
    _, package, raw = parse(core, document)
    fetch, verify = callbacks(payloads)
    assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) == 0
    base = tmp_path / "042731"
    (base / "current").rename(base / ".previous")
    (base / ".incoming").mkdir()
    (base / ".incoming/app.bin").write_bytes(b"interrupted")
    assert core.ideas_store_recover(root, b"042731") == 0
    assert sorted(p.name for p in base.iterdir()) == ["current"]
    assert (base / "current/app.bin").read_bytes() == payloads["app.bin"]


def test_multiple_ideas_survive_replacement_and_post_commit_cleanup(core, tmp_path):
    root = str(tmp_path).encode()
    for code, version in (("042731", "1.0"), ("042732", "1.0"), ("042731", "2.0")):
        document, payloads = descriptor(version, code)
        result, package, raw = parse(core, document, code.encode())
        assert result == 0
        fetch, verify = callbacks(payloads)
        assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) == 0
    # Simulate power loss after promotion but before removing the old backup.
    base = tmp_path / "042731"
    (base / ".previous").mkdir()
    (base / ".previous/old.bin").write_bytes(b"old version")
    assert core.ideas_store_recover(root, b"042731") == 0
    assert sorted(p.name for p in base.iterdir()) == ["current"]
    for code, version in (("042731", b"2.0"), ("042732", b"1.0")):
        saved = Package()
        assert core.ideas_store_read(root, code.encode(), C.byref(saved)) == 0
        assert saved.version == version


@pytest.mark.parametrize("case", ["short-code", "wrong-code", "path", "external-url", "uppercase-hash",
    "bootloader", "recovery", "wrong-offset", "overlap", "wrong-size", "extra-file", "duplicate-file",
    "duplicate-key", "trailing", "nul", "too-large", "wrong-layout", "signature", "fractional", "deep-json"])
def test_rejects_invalid_descriptor_before_download(core, case):
    doc, _ = descriptor()
    code = b"042731"
    if case == "short-code": code = b"42731"
    if case == "wrong-code": code = b"999999"
    if case == "path": doc["files"][0]["name"] = "../table.bin"
    if case == "external-url": doc["files"][0]["path"] = "https://other.example/file"
    if case == "uppercase-hash": doc["files"][0]["sha256"] = "A" * 64
    if case in ("bootloader", "recovery"): doc["system_manifest"]["components"][1]["kind"] = case
    if case == "wrong-offset": doc["system_manifest"]["components"][1]["target_offset"] = 0x20000
    if case == "overlap": doc["system_manifest"]["components"][1]["target_offset"] = 0x8000
    if case == "wrong-size": doc["files"][1]["size"] += 1
    if case == "extra-file": doc["files"].append(dict(name="x", path="/api/v1/ideas/files/x", size=1, sha256="a"*64))
    if case == "duplicate-file": doc["files"].append(doc["files"][0])
    if case == "duplicate-key": doc = json.dumps(doc).replace('"code":', '"code":"000000","code":').encode()
    if case == "trailing": doc = json.dumps(doc).encode() + b"false"
    if case == "nul": doc["application"]["name"] = "hidden\0suffix"
    if case == "too-large": doc = b" " * 16385
    if case == "wrong-layout": doc["system_manifest"]["target_layout_sha256"] = "a"*64
    if case == "signature": doc["system_manifest"]["signature"] = "unsupported"
    if case == "fractional": doc["files"][1]["size"] = 1.5
    if case == "deep-json": doc = b"[" * 2000 + b"0" + b"]" * 2000
    assert parse(core, doc, code)[0] != 0


def test_custom_assets_follow_atomic_download_and_offline_verification(core, tmp_path):
    document, payloads = descriptor()
    result, package, raw = parse(core, document)
    assert result == 0 and package.font_count == 2
    assert bytes(package.fonts[0]).rstrip(b"\0") == b"title.mif"
    fetch, verify = callbacks(payloads)
    root = str(tmp_path).encode()
    assert core.ideas_store_download(root, raw, len(raw), C.byref(package), fetch, verify, None) == 0
    assert core.ideas_store_verify(root, C.byref(package), verify, None) == 0
    (tmp_path / "042731/current/title.mif").write_bytes(b"corrupt")
    assert core.ideas_store_verify(root, C.byref(package), verify, None) != 0


@pytest.mark.parametrize("change", [
    lambda d: d.update(fonts=["title.mif", "missing.mif"]),
    lambda d: d.update(fonts=["title.mif", "title.mif"]),
    lambda d: d.update(fonts=["title.mif", "display.json"]),
    lambda d: d.update(fonts=["title.mif", "app.bin"]),
    lambda d: d.update(fonts=["title.mif", "body.mif", "extra.mif"]),
    lambda d: d.update(fonts=[]), lambda d: d.update(fonts="title.mif"),
    lambda d: d.update(presentation=""), lambda d: d.update(schema="mosaico-ideas/download/2"),
    lambda d: next(f for f in d["files"] if f["name"] == "body.mif").update(size=1024*1024),
    lambda d: next(f for f in d["files"] if f["name"] == "display.json").update(size=32769),
])
def test_custom_asset_dependency_graph_is_bounded(core, change):
    document, _ = descriptor(); change(document)
    assert parse(core, document)[0] != 0


def test_retired_gsp_descriptor_is_not_an_alternate_download_v1(core):
    document, _ = descriptor()
    fonts = document.pop("fonts")
    document["files"] = [f for f in document["files"] if f["name"] not in fonts]
    card = next(f for f in document["files"] if f["name"] == "display.json")
    card["name"] = document["presentation"] = "card.gspb"
    assert parse(core, document)[0] != 0
