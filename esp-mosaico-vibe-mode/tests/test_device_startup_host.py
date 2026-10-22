"""Verify nonblocking startup, peripheral degradation and motor shutdown."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

SANITIZERS = [] if os.name == "nt" else ["-fsanitize=address,undefined"]


@pytest.mark.parametrize("scenario", ["blocked", "sensor-failure", "task-failure", "retry", "imu-retry",
                                      "shutdown-active", "shutdown-paused",
                                      "shutdown-registration-failure", "motor-timer-failure",
                                      "wifi-sleep-retry", "wifi-wake-retry", "usb-lifecycle",
                                      "rotation-retry", "rotation-off-after-read-error",
                                      "rotation-imu-failure",
                                      "telemetry", "telemetry-battery-failure",
                                      "telemetry-task-failure", "telemetry-retry", "telemetry-transitions"])
def test_device_startup(tmp_path, scenario):
    root = Path(__file__).resolve().parents[1]
    component = root / "firmware/vibe-mode/components/app_device"
    fixture = root / "tests/host_device"
    for name in ("esp_err.h", "vibe_settings.h", "factory_network.h", "bsp/esp_mosaico.h",
                 "driver/gpio.h", "esp_attr.h", "esp_log.h", "esp_timer.h", "esp_system.h", "tusb.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    exe = tmp_path / "device"
    command = [
        os.environ.get("CC") or shutil.which("cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", *SANITIZERS, "-g", "-pthread",
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(component / "include"), "-I" + str(component / "private"),
        str(component / "src/vibe_device.c"), str(fixture / "main.c"), "-o", str(exe),
    ]
    macos_sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
    if macos_sdk.exists():
        command[1:1] = ["-isysroot", str(macos_sdk)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    result = subprocess.run([str(exe), scenario], check=False, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
