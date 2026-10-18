"""Exercise the actual bounded parser and NAND transaction using host files."""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent / "fixtures"))
from ideas_release import descriptor, encoded, ref

import pytest

# The host storage implementation exercises POSIX directory fsync and
# O_NOFOLLOW/lstat semantics; MinGW does not provide those contracts.
pytestmark = pytest.mark.skipif(os.name == "nt", reason="requires POSIX filesystem durability and link semantics")

RECOVERY = Path(__file__).resolve().parents[1] / "firmware/vibe-mode"


class File(C.Structure):
    _fields_ = [("name", C.c_char * 193), ("path", C.c_char * 193),
                ("sha256", C.c_char * 65), ("size", C.c_uint32)]


class Package(C.Structure):
    _fields_ = [("code", C.c_char * 7)] + [
        (key, C.c_char * 65) for key in ("application_id", "release_id", "version")
    ] + [("name", C.c_char * 129), ("description", C.c_char * 513),
         ("locale", C.c_char * 24)] + [
        (key, C.c_char * (193 if key in ("presentation", "icon") else 65)) for key in ("presentation", "icon", "application_sha256", "layout_sha256")
    ] + [(key, C.c_uint32) for key in ("application_size", "application_offset", "flash_size")] + [
        ("file_count", C.c_size_t), ("component_count", C.c_size_t),
        ("total_size", C.c_uint64), ("files", File * 32), ("manifest", C.c_char * 3073),
        ("font_count", C.c_size_t), ("fonts", (C.c_char * 193) * 2),
        ("root_sha256", C.c_char * 65), ("minimum_recovery_version", C.c_char * 65),
        ("default_locale", C.c_char * 8), ("root_size", C.c_size_t),
        ("display_index", C.c_uint * 2), ("asset_count", C.c_uint * 2),
        ("asset_index", (C.c_uint * 3) * 2), ("firmware_ready", C.c_bool)]


CALLBACK = C.CFUNCTYPE(C.c_int, C.POINTER(File), C.c_char_p, C.c_void_p)
PROGRESS = C.CFUNCTYPE(C.c_int, C.c_uint64, C.c_uint64, C.c_uint, C.c_uint, C.c_void_p)


@pytest.fixture(scope="module")
def core(tmp_path_factory):
    build = tmp_path_factory.mktemp("ideas-core")
    (build / "esp_err.h").write_text("""typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_CRC 0x109
""")
    dependency = Path(os.environ.get("CJSON_COMPONENT_DIR", str(RECOVERY / "managed_components/espressif__cjson")))
    cjson = next(dependency.rglob("cJSON.c"))
    result = subprocess.run([shutil.which("cc"), "-shared", "-fPIC", "-std=gnu11",
        "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-I" + str(build),
        "-I" + str(cjson.parent), "-I" + str(RECOVERY / "components/app_system/include"), "-I" + str(RECOVERY / "components/app_ideas/include"), "-I" + str(RECOVERY / "components/app_ideas/private"), str(RECOVERY / "components/app_ideas/src/ideas_package.c"),
        *[str(RECOVERY / "components/app_ideas/src" / (name + ".c")) for name in
          ("ideas_store", "ideas_integrity", "ideas_image", "ideas_font", "ideas_presentation")], "-lcrypto", "-lz", str(cjson), "-o", str(build / "core.so")],
        text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    lib = C.CDLL(str(build / "core.so"))
    lib.ideas_package_parse.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.POINTER(Package)]
    lib.ideas_package_firmware.argtypes = [C.POINTER(Package), C.c_char_p, C.c_size_t]
    lib.ideas_package_display.argtypes = [C.POINTER(Package), C.c_uint, C.c_char_p, C.c_size_t]
    lib.ideas_store_download.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t, C.POINTER(Package), CALLBACK, CALLBACK, C.c_void_p, C.c_void_p]
    lib.ideas_store_read.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_verify.argtypes = [C.c_char_p, C.POINTER(Package), CALLBACK, C.c_void_p]
    lib.ideas_store_refresh.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_prepare.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_prepare_progress.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(Package), PROGRESS, C.c_void_p]
    lib.ideas_store_finish.argtypes = [C.c_char_p, C.POINTER(Package)]
    lib.ideas_store_recover.argtypes = [C.c_char_p, C.c_char_p]
    return lib


def parse(core, document, code=b"042731", payloads=None):
    raw = document if isinstance(document, bytes) else encoded(document)
    package = Package()
    result = core.ideas_package_parse(raw, len(raw), code, C.byref(package))
    if result == 0 and payloads is not None:
        fw = payloads["firmware/manifest.json"]
        result = core.ideas_package_firmware(C.byref(package), fw, len(fw))
    return result, package, raw


def callbacks(payloads, fail=None, corrupt=False):
    @CALLBACK
    def fetch(file, path, _):
        name = file.contents.name.decode()
        Path(path.decode()).write_bytes(b"partial" if name == fail else payloads[name] + (b"bad" if corrupt else b""))
        return -1 if name == fail else 0

    @CALLBACK
    def verify(file, path, _):
        data = Path(path.decode()).read_bytes()
        return 0 if len(data) == file.contents.size and hashlib.sha256(data).hexdigest().encode() == file.contents.sha256 else -1
    return fetch, verify


def save(core, root, version="1.0.0", code="042731", fail=None, corrupt=False, change=None, stages=None, application_bytes=256):
    doc, payloads = descriptor(version, code, application_bytes)
    if change: change(doc, payloads)
    result, package, raw = parse(core, doc, code.encode(), payloads)
    assert result == 0
    fetch, verify = callbacks(payloads, fail=fail, corrupt=corrupt)
    stage = C.CFUNCTYPE(None, C.c_int, C.c_void_p)(lambda value, _: stages.append(value)) if stages is not None else None
    return core.ideas_store_download(str(root).encode(), raw, len(raw), C.byref(package), fetch, verify, None, stage), package, payloads


def test_single_version_replacement_and_offline_assets(core, tmp_path):
    for version in ("1.0.0", "2.0.0"):
        result, package, payloads = save(core, tmp_path, version)
        assert result == 0
        current = tmp_path / "042731/current"
        assert {str(p.relative_to(current)) for p in current.rglob("*") if p.is_file()} == set(payloads) | {"manifest.json"}
        assert (current / "firmware/manifest.json").read_bytes() == payloads["firmware/manifest.json"]
        assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") == 0
        assert sorted(p.name for p in current.parent.iterdir()) == [".roots", "current"]
        saved = Package()
        assert core.ideas_store_read(str(tmp_path).encode(), b"042731", C.byref(saved)) == 0
        assert saved.version.decode() == version and saved.name.decode() == "测试"
        for language, locale, text in ((0, "zh-CN", "测试"), (1, "en", "Test")):
            display = payloads[f"device/{locale}/display.json"]
            assert core.ideas_package_display(C.byref(saved), language, display, len(display)) == 0
            assert saved.name.decode() == text


@pytest.mark.parametrize("failure", ["network", "hash"])
def test_failed_update_keeps_previous_download(core, tmp_path, failure):
    assert save(core, tmp_path)[0] == 0
    assert save(core, tmp_path, "2.0.0", fail="firmware/app.bin" if failure == "network" else None, corrupt=failure == "hash")[0] != 0
    saved = Package()
    assert core.ideas_store_read(str(tmp_path).encode(), b"042731", C.byref(saved)) == 0
    assert saved.version == b"1.0.0"


def test_power_loss_between_generation_renames(core, tmp_path):
    _, _, payloads = save(core, tmp_path)
    base = tmp_path / "042731"
    (base / "current").rename(base / ".previous")
    (base / ".incoming/firmware").mkdir(parents=True)
    (base / ".incoming/firmware/app.bin").write_bytes(b"interrupted")
    assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") == 0
    assert (base / "current/firmware/app.bin").read_bytes() == payloads["firmware/app.bin"]
    assert not (base / ".incoming").exists()


def test_prepare_reports_all_verified_bytes(core, tmp_path):
    result, package, _ = save(core, tmp_path, application_bytes=65536)
    assert result == 0
    samples = []

    @PROGRESS
    def progress(checked, total, verified, count, _):
        samples.append((checked, total, verified, count))
        return 0

    assert core.ideas_store_prepare_progress(
        str(tmp_path).encode(), b"042731", C.byref(package), progress, None) == 0
    total = package.total_size - package.root_size
    assert samples[-1] == (total, total, package.file_count, package.file_count)
    assert all(a[0] <= b[0] <= b[1] for a, b in zip(samples, samples[1:]))
    assert sum(a[0] < b[0] for a, b in zip(samples, samples[1:])) > package.file_count


def test_cancel_prepare_preserves_generations_without_fallback(core, tmp_path):
    assert save(core, tmp_path)[0] == 0
    _, package, _ = save(core, tmp_path, "2.0.0", application_bytes=65536)
    assert (tmp_path / "042731/.previous").is_dir()
    before = {p.relative_to(tmp_path): p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}
    samples = []
    cancelled = False

    @PROGRESS
    def progress(checked, total, verified, count, _):
        nonlocal cancelled
        samples.append((checked, total))
        if checked >= 16384 and not cancelled:
            cancelled = True
            return 0x103
        return 0  # An erroneous retry of .previous would succeed.

    assert core.ideas_store_prepare_progress(
        str(tmp_path).encode(), b"042731", C.byref(package), progress, None) == 0x103
    assert cancelled and 16384 <= samples[-1][0] < samples[-1][1]
    assert all(checked < 16384 for checked, _ in samples[:-1])
    assert before == {p.relative_to(tmp_path): p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}


def test_corrupt_new_generation_falls_back_to_verified_previous(core, tmp_path):
    assert save(core, tmp_path)[0] == 0
    assert save(core, tmp_path, "2.0.0")[0] == 0
    current = tmp_path / "042731/current"
    (current / "device/en/font.miff").write_bytes(b"corrupt")
    assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") == 0
    assert json.loads((current / "firmware/manifest.json").read_bytes())["release"] == "1.0.0"


def test_root_digest_detects_coordinated_manifest_and_file_corruption(core, tmp_path):
    assert save(core, tmp_path)[0] == 0
    current = tmp_path / "042731/current"
    root = json.loads((current / "manifest.json").read_bytes())
    root["release_id"] = "different-snapshot"
    (current / "manifest.json").write_bytes(encoded(root))
    assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") != 0
    assert current.is_dir()  # Do not destroy the only copy on failed validation.


def test_multiple_ideas_and_post_commit_recovery(core, tmp_path):
    for code, version in (("042731", "1.0.0"), ("042732", "1.0.0"), ("042731", "2.0.0")):
        assert save(core, tmp_path, version, code)[0] == 0
    assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") == 0
    for code, version in (("042731", b"2.0.0"), ("042732", b"1.0.0")):
        saved = Package()
        assert core.ideas_store_read(str(tmp_path).encode(), code.encode(), C.byref(saved)) == 0
        assert saved.version == version


@pytest.mark.parametrize("case", ["wrong-code", "path", "uppercase-hash", "unknown-key", "duplicate-key",
    "trailing", "nul", "large", "fractional", "deep", "locale", "duplicate-locale", "extra-asset", "case-collision", "legacy", "invalid-utf8"])
def test_rejects_invalid_root_before_download(core, case):
    doc, _ = descriptor()
    view = doc["device"]["presentations"][0]
    if case == "wrong-code": doc["application"]["code"] = "999999"
    if case == "path": view["assets"][0]["path"] = "device/zh-CN/../font.miff"
    if case == "uppercase-hash": view["assets"][0]["sha256"] = "A" * 64
    if case == "unknown-key": doc["name"] = "redundant"
    if case == "duplicate-key": doc = encoded(doc).replace(b'"code":', b'"code":"000000","code":')
    if case == "trailing": doc = encoded(doc) + b"false"
    if case == "nul": doc["application"]["id"] = "hidden\0suffix"
    if case == "large": doc = b" " * 65537
    if case == "fractional": view["display"]["size"] = 1.5
    if case == "deep": doc = b"[" * 2000 + b"0" + b"]" * 2000
    if case == "locale": doc["device"]["default_locale"] = "fr"
    if case == "duplicate-locale": doc["device"]["presentations"][1] = view
    if case == "extra-asset": view["assets"] *= 2
    if case == "case-collision": view["assets"][1]["path"] = view["assets"][0]["path"].replace("font", "FONT")
    if case == "legacy": doc["schema"] = "mosaico-ideas/download/1"
    if case == "invalid-utf8": doc = encoded(doc).replace(b"stable-app-id", b"invalid-\xff")
    assert parse(core, doc)[0] != 0


@pytest.mark.parametrize("case", ["bootloader", "recovery", "offset", "layout", "signature", "unknown", "overlap", "flags", "nested", "hash"])
def test_rejects_invalid_inner_manifest(core, case):
    doc, files = descriptor()
    fw = json.loads(files["firmware/manifest.json"]); c = fw["components"][1]
    if case in ("bootloader", "recovery"): c["kind"] = case
    if case == "offset": c["target_offset"] = 0x20000
    if case == "layout": fw["target_layout_sha256"] = "a" * 64
    if case == "signature": fw["signature"] = "unsupported"
    if case == "unknown": c["extra"] = 1
    if case == "overlap": c["target_offset"] = 0x8000
    if case == "flags": c["flags"] = 1
    if case == "nested": c["file"] = "folder/app.bin"
    files["firmware/manifest.json"] = encoded(fw)
    doc["firmware"]["manifest"] = ref("firmware/manifest.json", files["firmware/manifest.json"])
    if case == "hash": files["firmware/manifest.json"] += b" "
    assert parse(core, doc, payloads=files)[0] != 0


@pytest.mark.parametrize("case", ["glyph", "font", "png", "dependency"])
def test_inactive_locale_must_be_valid_before_commit(core, tmp_path, case):
    assert save(core, tmp_path)[0] == 0
    def change(doc, files):
        view = doc["device"]["presentations"][1]
        display = json.loads(files["device/en/display.json"])
        if case == "glyph": display["name"]["glyphs"][0][0] = 999
        if case == "font": files["device/en/font.miff"] = b"invalid-font"
        if case == "png": files["device/en/icon.png"] = b"invalid-png"
        if case == "dependency": display["icon"] = "missing.png"
        files["device/en/display.json"] = encoded(display)
        view["display"] = ref("device/en/display.json", files["device/en/display.json"])
        view["assets"] = [ref(a["path"], files[a["path"]]) for a in view["assets"]]
    assert save(core, tmp_path, "2.0.0", change=change)[0] != 0
    saved = Package()
    assert core.ideas_store_read(str(tmp_path).encode(), b"042731", C.byref(saved)) == 0
    assert saved.version == b"1.0.0"


def test_unexpected_files_and_links_are_not_trusted(core, tmp_path):
    assert save(core, tmp_path)[0] == 0
    base = tmp_path / "042731/current"
    (base / "extra").write_text("unlisted")
    saved = Package()
    assert core.ideas_store_read(str(tmp_path).encode(), b"042731", C.byref(saved)) != 0
    (base / "extra").unlink()
    original = (base / "device/en/font.miff").read_bytes()
    external = tmp_path / "outside-font"; external.write_bytes(original)
    (base / "device/en/font.miff").unlink(); (base / "device/en/font.miff").symlink_to(external)
    assert core.ideas_store_read(str(tmp_path).encode(), b"042731", C.byref(saved)) != 0
    assert core.ideas_store_recover(str(tmp_path).encode(), b"042731") != 0
    assert external.read_bytes() == original


def test_metadata_refresh_never_authorizes_corrupt_install(core, tmp_path):
    result, package, _ = save(core, tmp_path)
    assert result == 0
    root = str(tmp_path).encode()
    assert core.ideas_store_finish(root, C.byref(package)) == 0
    current = tmp_path / "042731/current"
    (current / "firmware/app.bin").write_bytes(b"corrupt")
    saved = Package()
    assert core.ideas_store_refresh(root, b"042731", C.byref(saved)) == 0
    assert core.ideas_store_prepare(root, b"042731", C.byref(saved)) != 0
    assert current.is_dir()


def test_refresh_recovers_interrupted_generation(core, tmp_path):
    assert save(core, tmp_path)[0] == 0
    assert save(core, tmp_path, "2.0.0")[0] == 0
    current = tmp_path / "042731/current"
    (current / "firmware/app.bin").write_bytes(b"corrupt")
    saved = Package()
    assert core.ideas_store_refresh(str(tmp_path).encode(), b"042731", C.byref(saved)) == 0
    assert saved.version == b"1.0.0"
    assert sorted(p.name for p in current.parent.iterdir()) == [".roots", "current"]


@pytest.mark.parametrize("failure,expected", [(None, [0, 1, 2]), ("network", []), ("hash", [0])])
def test_download_stages_advance_only_after_success(core, tmp_path, failure, expected):
    stages = []
    result, _, _ = save(core, tmp_path, fail="firmware/app.bin" if failure == "network" else None,
                        corrupt=failure == "hash", stages=stages)
    assert (result == 0) == (failure is None)
    assert stages == expected


def test_first_download_failure_and_interrupted_delete_leave_no_idea(core, tmp_path):
    assert save(core, tmp_path, fail="firmware/app.bin")[0] != 0
    base = tmp_path / "042731"
    assert not base.exists()
    assert save(core, tmp_path)[0] == 0
    assert save(core, tmp_path, "2.0.0")[0] == 0  # a previous generation also exists
    (base / "current").rename(base / ".deleted")
    saved = Package()
    assert core.ideas_store_refresh(str(tmp_path).encode(), b"042731", C.byref(saved)) != 0
    assert not base.exists()
