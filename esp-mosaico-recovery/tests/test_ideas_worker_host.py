"""Run the real Ideas service against immutable releases and a temporary NAND.

No published Idea, network connection, device or consuming workspace is needed.
Only SDK and adjacent component boundaries are faked; all nine app_ideas source
files compile separately and scenarios call its public service interface.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ROOT / "firmware/recovery/components"
FIXTURE = ROOT / "tests/host_ideas"
SCENARIOS = [
    "preview", "cancel-queued", "cancel-resolve", "cancel-asset", "fixed-release",
    "task-failure", "reject-bad-font", "reject-bad-display", "reject-bad-icon",
    "reject-hash", "reject-truncated", "reject-missing", "reject-redirect",
    "reject-read-error", "reject-wrong-length", "reject-unknown-code", "reject-resolve-truncated",
    "reject-bad-descriptor", "reject-bad-flash", "rollback-hash", "rollback-truncated",
    "offline", "install-corrupt", "install-commit-failure", "boot-identity",
    "interrupted-generation", "admission", "repair-current",
]


def png(width=2):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, 2, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress((b"\0" + b"\xff\0\0" * width) * 2)) +
            chunk(b"IEND", b""))


def write_release(directory, release, version="1.0"):
    font = (struct.pack("<4sHHIHHHHI", b"MIFF", 1, 24, 50, 1, 20, 24, 0, 24) +
            struct.pack("<IIHHhhi", 42, 44, 2, 3, 0, 3, 128) +
            bytes([255, 128, 0, 255, 255, 0]))
    display = dict(schema="mosaico-ideas/display/1", locale="en", icon="icon.png",
                   name=dict(text="A", font="font.mif", width=328, height=84, glyphs=[[42, 0, 3]]),
                   description=dict(text="B", font="font.mif", width=416, height=176, glyphs=[[42, 4, 5]]))
    # Rehash malformed assets so semantic rejection is tested independently of SHA checks.
    if release == "bad-font":
        font = b"XXXX" + font[4:]
    if release == "bad-display":
        display["name"]["glyphs"] = [[42, 327, 3]]
    payloads = {"table.bin": b"t" * 4096, "app.bin": version.encode() * 300,
                "display.json": json.dumps(display).encode(), "font.mif": font,
                "icon.png": png(97 if release == "bad-icon" else 2)}
    files = []
    for name, data in payloads.items():
        immutable_name = release + "-" + name
        (directory / immutable_name).write_bytes(data)
        files.append(dict(name=name, path="/api/v1/ideas/files/" + immutable_name,
                          size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    components = [dict(id=i + 1, kind=kind, target_offset=offset, file=f["name"],
                       size=f["size"], sha256=f["sha256"])
                  for i, (f, kind, offset) in enumerate(zip(
                      files, ("partition_table", "application"), (0x8000, 0x210000)))]
    document = dict(
        schema="mosaico-ideas/download/1",
        application=dict(code="042731", id="stable-app-id", release_id="release-" + version,
                         version=version, name="A", description="B", locale="en"),
        system_manifest=dict(schema="esp-iris-system-update/0.2", release=version,
                             target=dict(chip_id=32, flash_size=16 * 1024 * 1024),
                             target_layout_sha256=files[0]["sha256"], components=components),
        files=files, presentation="display.json", fonts=["font.mif"], icon="icon.png")
    if release == "bad-flash":
        document["system_manifest"]["target"]["flash_size"] = 8 * 1024 * 1024
    (directory / (release + ".json")).write_text(
        "{broken" if release == "bad-descriptor" else json.dumps(document))


@pytest.fixture(scope="module")
def worker(tmp_path_factory):
    build = tmp_path_factory.mktemp("ideas-worker")
    cc = shutil.which("cc")
    assert cc, "Host C compiler is required"
    cjson = Path(os.environ.get("CJSON_COMPONENT_DIR", str(
        ROOT / "firmware/recovery/managed_components/espressif__cjson")))
    sources = list(cjson.rglob("cJSON.c"))
    assert sources, "Build Recovery first or set CJSON_COMPONENT_DIR"
    cjson_source = sources[0]
    for name in ("esp_err.h", "esp_attr.h", "esp_crt_bundle.h", "esp_flash.h", "esp_heap_caps.h",
                 "esp_http_client.h", "esp_ota_ops.h", "esp_system.h", "esp_timer.h", "nvs.h",
                 "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "psa/crypto.h"):
        header = build / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    executable = build / "ideas-worker"
    command = [cc, "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-sign-compare", "-Wno-deprecated-declarations",
               "-I" + str(build), "-I" + str(FIXTURE), "-I" + str(cjson_source.parent),
               "-I" + str(ROOT / "include"),
               "-I" + str(ROOT.parent / "ESP-Iris/components/esp_iris/include"),
               *["-I" + str(COMPONENTS / name / "include") for name in (
                   "app_ideas", "app_storage", "app_nand_update", "app_network", "app_system", "app_system_update")],
               "-I" + str(COMPONENTS / "app_ideas/private"),
               *map(str, sorted((COMPONENTS / "app_ideas/src").glob("*.c"))),
               str(FIXTURE / "sdk.c"), str(FIXTURE / "main.c"), str(cjson_source),
               *["-Wl,--wrap=" + name for name in ("fopen", "stat", "opendir", "mkdir", "unlink", "rmdir", "rename")],
               "-lcrypto", "-o", str(executable)]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    fixtures = build / "releases"
    fixtures.mkdir()
    for release in ("v1", "v2", "bad-font", "bad-display", "bad-icon", "bad-descriptor", "bad-flash"):
        write_release(fixtures, release, "2.0" if release == "v2" else "1.0")
    return executable, fixtures


@pytest.mark.skipif(sys.platform != "linux", reason="Linux fixture requires OpenSSL and linker wrappers")
@pytest.mark.parametrize("scenario", SCENARIOS)
def test_ideas_service_functionality(worker, tmp_path, scenario):
    executable, fixtures = worker
    result = subprocess.run([str(executable), scenario, str(fixtures), str(tmp_path)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
