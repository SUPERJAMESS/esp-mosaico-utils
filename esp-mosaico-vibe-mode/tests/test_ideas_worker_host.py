"""Run the real Ideas service against immutable releases and a temporary NAND.

No published Idea, network connection, device or consuming workspace is needed.
Only SDK and adjacent component boundaries are faked; all app_ideas source
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
COMPONENTS = ROOT / "firmware/vibe-mode/components"
FIXTURE = ROOT / "tests/host_ideas"
BAD_MULTIPART = ["missing", "duplicate", "path", "locale", "length", "oversize", "hash", "epilogue", "header", "media"]
SCENARIOS = [
    "download-stages", "retry-manifest", "retry-read", "retry-limit", "bilingual-offline",
    "preview", "preview-zh", "preview-single-byte", "cancel-queued", "cancel-resolve", "cancel-asset", "fixed-release",
    "queue-failure", "init-retry", "worker-reuse", "worker-handoff", "reject-bad-font", "reject-bad-display", "reject-bad-icon",
    "reject-hash", "reject-etag", "reject-truncated", "reject-missing", "reject-redirect",
    "reject-read-error", "reject-wrong-length", "reject-unknown-code", "reject-resolve-truncated",
    "reject-bad-descriptor", "reject-bad-flash", "rollback-hash", "rollback-truncated",
    "offline", "install-corrupt", "install-commit-failure", "boot-identity",
    "interrupted-generation", "admission", "repair-current", "io-budget",
    "open", "open-progress", "open-cancel-block", "open-cancel-current", "open-cancel-queued", "open-cancel-gate",
    "delete", "delete-current", "download-cancel", "batch", "development", "development-ota-busy", "scan-metadata-only", "refresh-updates", "open-cancel", "no-space",
] + ["reject-bad-multipart-" + kind for kind in BAD_MULTIPART]


sys.path.insert(0, str(ROOT / "tests/fixtures"))
sys.path.insert(0, str(ROOT.parent / "mosaico-tools/tools"))
from mosaico_cli.ideas_preview import preview_response
from ideas_release import descriptor, encoded, ref, png


def write_release(directory, release, version="1.0"):
    document, payloads = descriptor(version, application_bytes=65536)
    display_path = "device/en/display.json"
    display = json.loads(payloads[display_path])
    if release == "bad-font": payloads["device/en/font.miff"] = b"bad-font"
    if release == "bad-display": display["name"]["glyphs"] = [[1, 400, 1]]
    if release == "bad-icon": payloads["device/en/icon.png"] = png(97)
    payloads[display_path] = encoded(display)
    for view in document["device"]["presentations"]:
        view["display"] = ref(view["display"]["path"], payloads[view["display"]["path"]])
        view["assets"] = [ref(a["path"], payloads[a["path"]]) for a in view["assets"]]
    if release == "bad-flash":
        fw = json.loads(payloads["firmware/manifest.json"]); fw["target"]["flash_size"] = 8*1024*1024
        payloads["firmware/manifest.json"] = encoded(fw)
    document["firmware"]["manifest"] = ref("firmware/manifest.json", payloads["firmware/manifest.json"])
    index = {}
    for path, data in payloads.items():
        sha = hashlib.sha256(data).hexdigest()
        (directory / sha).write_bytes(data)
        alias = "inner.json" if path == "firmware/manifest.json" else path.split("/")[-1]
        index[sha] = dict(alias=alias, firmware=path.startswith("firmware/") and path != "firmware/manifest.json")
    (directory / (release + "-index.json")).write_bytes(encoded(index))
    (directory / (release + ".json")).write_bytes(encoded(document))
    blobs = {hashlib.sha256(data).hexdigest(): data for data in payloads.values()}
    for locale in ("en", "zh-CN"):
        wire, media = preview_response(encoded(document), locale, blobs.__getitem__)
        if release == "bad-descriptor":
            wire = wire.replace(encoded(document), b"!" + encoded(document)[1:], 1)
        if release.startswith("bad-multipart-"):
            kind = release[len("bad-multipart-"):]
            marker = b"--" + media.split('boundary="')[1][:-1].encode()
            pieces = wire.split(marker)
            if kind == "missing": wire = marker.join(pieces[:-2] + pieces[-1:])
            elif kind == "duplicate": wire = marker.join(pieces[:2] + pieces[1:])
            elif kind == "path": wire = wire.replace(b"Content-Location: firmware/", b"Content-Location: ../", 1)
            elif kind == "locale": wire = wire.replace(f"Content-Location: device/{locale}/".encode(), b"Content-Location: device/fr/", 1)
            elif kind == "length": wire = wire.replace(f"Content-Length: {len(encoded(document))}".encode(), f"Content-Length: {len(encoded(document))+1}".encode(), 1)
            elif kind == "oversize": wire = wire.replace(f"Content-Length: {len(encoded(document))}".encode(), b"Content-Length: 65537", 1)
            elif kind == "hash": wire = wire.replace(payloads["firmware/manifest.json"], payloads["firmware/manifest.json"].replace(b'"release":"1.0"', b'"release":"9.0"'), 1)
            elif kind == "epilogue": wire += b"unexpected"
            elif kind == "header": wire = wire.replace(b"Content-Type: application/json", b"Content-Type: application/json\r\nContent-Type: application/json", 1)
            elif kind == "media": media = media.replace('type="application/json"', 'type="application/json"; type="application/json"')
        (directory / f"{release}-{locale}.multipart").write_bytes(wire)
        (directory / f"{release}-{locale}.media").write_text(media)



@pytest.fixture(scope="module")
def worker(tmp_path_factory):
    build = tmp_path_factory.mktemp("ideas-worker")
    cc = shutil.which("cc")
    assert cc, "Host C compiler is required"
    cjson = Path(os.environ.get("CJSON_COMPONENT_DIR", str(
        ROOT / "firmware/vibe-mode/managed_components/espressif__cjson")))
    sources = list(cjson.rglob("cJSON.c"))
    assert sources, "Build Recovery first or set CJSON_COMPONENT_DIR"
    cjson_source = sources[0]
    for name in ("esp_err.h", "esp_attr.h", "esp_crt_bundle.h", "esp_flash.h", "esp_heap_caps.h",
                 "esp_http_client.h", "esp_random.h", "esp_app_desc.h", "mosaico_boot.h", "esp_partition.h", "esp_image_format.h", "esp_flash_partitions.h", "esp_ota_ops.h", "esp_system.h", "esp_timer.h", "nvs.h",
                 "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "freertos/queue.h", "psa/crypto.h"):
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
                   "app_ideas", "app_settings", "app_storage", "app_nand_update", "app_network", "app_system", "app_system_update")],
               "-I" + str(COMPONENTS / "app_ideas/private"),
               *map(str, sorted((COMPONENTS / "app_ideas/src").glob("*.c"))),
               str(FIXTURE / "sdk.c"), str(FIXTURE / "main.c"), str(cjson_source),
               *["-Wl,--wrap=" + name for name in ("calloc", "fopen", "stat", "lstat", "open", "opendir", "mkdir", "unlink", "rmdir", "rename")],
               "-lcrypto", "-lz", "-pthread", "-o", str(executable)]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    fixtures = build / "releases"
    fixtures.mkdir()
    for release in ["v1", "v2", "bad-font", "bad-display", "bad-icon", "bad-descriptor", "bad-flash", *["bad-multipart-" + k for k in BAD_MULTIPART]]:
        write_release(fixtures, release, "2.0" if release == "v2" else "1.0")
    for release in ("v1", "v2"):
        document = json.loads((fixtures / (release + ".json")).read_text())
        document["application"].update(code="042732", id="stable-app-id-2")
        (fixtures / (release + "-042732.json")).write_text(json.dumps(document))
        for locale in ("en", "zh-CN"):
            wire, media = preview_response(encoded(document), locale, lambda sha: (fixtures / sha).read_bytes())
            (fixtures / f"{release}-042732-{locale}.multipart").write_bytes(wire)
            (fixtures / f"{release}-042732-{locale}.media").write_text(media)
    return executable, fixtures


@pytest.mark.skipif(sys.platform != "linux", reason="Linux fixture requires OpenSSL and linker wrappers")
@pytest.mark.parametrize("scenario", SCENARIOS)
def test_ideas_service_functionality(worker, tmp_path, scenario):
    executable, fixtures = worker
    result = subprocess.run([str(executable), scenario, str(fixtures), str(tmp_path)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
