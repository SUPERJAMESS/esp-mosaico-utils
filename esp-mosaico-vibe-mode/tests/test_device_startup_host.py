"""Keys and brightness must remain usable while optional drivers block/fail."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

SANITIZERS = [] if os.name == "nt" else ["-fsanitize=address,undefined"]

@pytest.mark.parametrize("scenario", ["blocked", "sensor-failure", "task-failure", "retry"])
def test_device_startup(tmp_path, scenario):
    root = Path(__file__).resolve().parents[1]
    component = root / "firmware/vibe-mode/components/app_device"
    fixture = root / "tests/host_device"
    for name in ("esp_err.h", "vibe_settings.h", "factory_network.h", "bsp/esp_mosaico.h",
                 "driver/gpio.h", "esp_attr.h", "esp_log.h", "esp_timer.h", "tusb.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    exe = tmp_path / "device"
    subprocess.run([
        shutil.which("cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", *SANITIZERS, "-g", "-pthread",
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(component / "include"),
        str(component / "src/vibe_device.c"), str(fixture / "main.c"), "-o", str(exe),
    ], check=True, capture_output=True, text=True)
    result = subprocess.run([str(exe), scenario], check=False, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
