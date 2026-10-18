"""Exercise the production cache against a failing, mutable block device."""
from pathlib import Path
import os
import shutil
import subprocess

SANITIZERS = [] if os.name == "nt" else ["-fsanitize=address,undefined"]


def test_storage_cache(tmp_path):
    root = Path(__file__).resolve().parents[1]
    component = root / "firmware/vibe-mode/components/app_storage"
    fixture = root / "tests/host_storage"
    headers = ["esp_blockdev.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h",
               "freertos/FreeRTOS.h", "freertos/semphr.h", "driver/spi_master.h"]
    for name in headers:
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    exe = tmp_path / "cache"
    result = subprocess.run([
        shutil.which("cc"), "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-Werror", *SANITIZERS, "-g", "-pthread",
        "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(component / "private"),
        str(component / "src/storage_blockdev.c"), str(fixture / "cache.c"), "-o", str(exe),
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr


def test_storage_spi_routing(tmp_path):
    root = Path(__file__).resolve().parents[1]
    component = root / "firmware/vibe-mode/components/app_storage"
    fixture = root / "tests/host_storage"
    for name in ("esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h", "driver/spi_master.h"):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    for polling in (0, 1):
        (tmp_path / "sdkconfig.h").write_text(
            "#define CONFIG_IRIS_FACTORY_NAND_SYSTEM_UPDATE 1\n"
            f"#define CONFIG_IRIS_FACTORY_NAND_POLL_SHORT {polling}\n"
        )
        exe = tmp_path / f"spi-{polling}"
        result = subprocess.run([
            shutil.which("cc"), "-std=gnu11", "-Werror=implicit-function-declaration",
            *SANITIZERS, "-g", "-pthread",
            f'-DSTORAGE_METRICS_SOURCE="{(component / "src/storage_metrics.c").as_posix()}"',
            "-I" + str(tmp_path), "-I" + str(fixture), "-I" + str(component / "private"),
            str(fixture / "spi.c"), "-o", str(exe),
        ], text=True, capture_output=True)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
