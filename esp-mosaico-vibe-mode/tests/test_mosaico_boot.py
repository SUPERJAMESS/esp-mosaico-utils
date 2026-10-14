"""Exercise ABI 2 boot intent against allocation/storage/image failures."""
import importlib.util
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_boot_intent_reader_with_multiple_active_pages(tmp_path):
    fixture = ROOT / "tests/host_boot"
    for name in ("esp_err.h", "nvs_bootloader_private.h"):
        (tmp_path / name).write_text('#include "nvs_reader_sdk.h"\n')
    (tmp_path / "esp_log.h").write_text('#define ESP_LOGE(...) ((void)0)\n')
    source = ROOT / "firmware/vibe-mode/bootloader_components/main/mosaico_boot_nvs.c"
    output = tmp_path / "nvs-reader-test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        '-DREADER_SOURCE="' + str(source) + '"',
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(ROOT / "include"),
        str(fixture / "nvs_reader.c"), "-o", str(output),
    ], check=True, capture_output=True, timeout=30)
    subprocess.run([str(output)], check=True, capture_output=True, timeout=10)


def test_boot_intent_runtime(tmp_path):
    source = ROOT / "components/esp_mosaico_boot/mosaico_boot.c"
    fixture = ROOT / "tests/host_boot"
    for name in ("esp_err.h", "esp_image_format.h", "esp_iris.h", "esp_ota_ops.h",
                 "esp_partition.h", "nvs.h", "nvs_flash.h"):
        (tmp_path / name).write_text('#include "sdk.h"\n')
    (tmp_path / "esp_log.h").write_text('#define ESP_LOGI(tag, ...) ((void)(tag))\n')
    output = tmp_path / "boot-test"
    result = subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(ROOT / "include"),
        "-I" + str(ROOT / "components/esp_mosaico_boot/include"),
        str(source), str(fixture / "main.c"), "-o", str(output),
    ], capture_output=True, text=True, check=False, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(output)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr


def test_installer_bootstrap_matches_public_abi():
    spec = importlib.util.spec_from_file_location(
        "boot_intent", ROOT / "firmware/vibe-mode/tools/boot_intent.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    header = (ROOT / "include/mosaico_recovery_contract.h").read_text()
    marker = re.search(r'#define MOSAICO_BOOTSTRAP_MARKER "([^"]+)"', header)[1].encode()
    image = module.bootstrap_image()
    assert len(image) == 0x2000
    assert image[32:48] == marker
    assert image[:32] + image[48:] == b"\xff" * (0x2000 - 16)


def test_bootloader_selects_test_without_mutating_flash(tmp_path):
    source = ROOT / "firmware/vibe-mode/bootloader_components/main/bootloader_start.c"
    fixture = ROOT / "tests/host_boot"
    for name in re.findall(r'#include "([^"]+)"', source.read_text()) + ["sys/reent.h"]:
        if name == "mosaico_recovery_contract.h":
            continue
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "selector_sdk.h"\n')
    # Copy the source so its quoted splash header resolves to the deterministic fake.
    copied = tmp_path / "selector_under_test.c"
    copied.write_text(source.read_text())
    output = tmp_path / "selector-test"
    result = subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        '-DSELECTOR_SOURCE="' + str(copied) + '"',
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(ROOT / "include"),
        str(fixture / "selector.c"), "-o", str(output),
    ], capture_output=True, text=True, check=False, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(output)], check=True, capture_output=True, timeout=10)
