"""Persistent settings and interrupted reset, using the actual service/API."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_settings_and_reset_preserve_identity_and_nand(tmp_path):
    fixture = ROOT / "tests/host_settings"
    source = ROOT / "firmware/vibe-mode/components/app_settings/src/vibe_settings.c"
    for name in ("esp_err.h", "esp_attr.h", "esp_system.h", "nvs.h", "nvs_flash.h",
                 "factory_network.h", "factory_locale.h", "factory_system_metadata.h", "mosaico_boot.h",
                 "factory_system_update.h", "app_storage.h", "freertos/FreeRTOS.h",
                 "freertos/semphr.h", "freertos/task.h"):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    executable = tmp_path / "settings-test"
    result = subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", '-DSOURCE="' + str(source) + '"',
        '-DLOCALE_SOURCE="' + str(ROOT / "firmware/vibe-mode/components/app_system/src/factory_locale.c") + '"',
        "-I" + str(tmp_path), "-I" + str(fixture),
        "-I" + str(source.parents[1] / "include"),
        "-I" + str(ROOT / "components/esp_mosaico_app_recovery/include"),
        str(fixture / "main.c"), str(ROOT / "components/esp_mosaico_app_recovery/mosaico_shared.c"),
        "-o", str(executable)], capture_output=True, text=True, timeout=30, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
