"""Exercise transient editor pages and reserved credentials with the real settings service."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from PIL import Image
from test_recovery_ui_host import native_build

__all__ = ["native_build"]

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def settings_ui(tmp_path_factory, native_build):
    build = tmp_path_factory.mktemp("settings-ui")
    fixture = ROOT / "tests/host_settings"
    components = ROOT / "firmware/vibe-mode/components"
    for name in ("esp_err.h", "esp_attr.h", "esp_system.h", "nvs.h", "nvs_flash.h",
                 "factory_network.h", "factory_locale.h", "factory_system_metadata.h",
                 "mosaico_boot.h", "factory_system_update.h", "app_storage.h",
                 "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
        header = build / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    library = native_build[0]
    executable = build / ("settings-ui.exe" if os.name == "nt" else "settings-ui")
    command = ["cc", "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               '-DSOURCE="' + (components / "app_settings/src/vibe_settings.c").as_posix() + '"',
               '-DLOCALE_SOURCE="' + (components / "app_system/src/factory_locale.c").as_posix() + '"',
               "-I" + str(build), "-I" + str(fixture),
               "-I" + str(components / "app_ui/private"),
               *["-I" + str(components / name / "include")
                 for name in ("app_ui", "app_settings", "app_device", "app_network")],
               "-I" + str(ROOT / "components/esp_mosaico_app_recovery/include"),
               str(fixture / "ui.c"),
               str(ROOT / "components/esp_mosaico_app_recovery/mosaico_shared.c"),
               str(library), "-o", str(executable)]
    if os.name != "nt":
        command += ["-Wl,-rpath," + str(library.parent)]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if sys.platform == "linux":
            command += ["-no-pie"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    if os.name == "nt":
        shutil.copy2(library, build / library.name)
    return executable


@pytest.mark.parametrize("scenario", ["boot", "sleep", "boot-sleep", "boot-cancel",
                                      "ssid", "password-hex", "password-short"])
def test_shared_editor_keeps_drafts_and_valid_credentials(settings_ui, tmp_path, scenario):
    result = subprocess.run([str(settings_ui), scenario, str(tmp_path)],
                            capture_output=True, text=True, timeout=30, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    if scenario in ("boot", "sleep", "boot-sleep"):
        with Image.open(tmp_path / "before.ppm") as before, Image.open(tmp_path / "returned.ppm") as returned:
            assert before.crop((16, 96, 464, 160)).tobytes() == returned.crop((16, 96, 464, 160)).tobytes()


@pytest.mark.parametrize("language", ["en", "zh"])
def test_reset_paints_success_before_restarting(settings_ui, tmp_path, language):
    result = subprocess.run([str(settings_ui), "reset-" + language, str(tmp_path)],
                            capture_output=True, text=True, timeout=30, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    with Image.open(tmp_path / "reset-clearing.ppm") as clearing, Image.open(tmp_path / "reset-success.ppm") as success:
        assert clearing.crop((24, 170, 456, 310)).tobytes() != success.crop((24, 170, 456, 310)).tobytes()
        assert success.getbbox()  # The success frame is present before esp_restart.
