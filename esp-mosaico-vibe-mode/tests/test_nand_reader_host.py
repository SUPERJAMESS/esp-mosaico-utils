"""Internal allocation must precede NOR erase; exercise real chunk/tail/failure paths."""
from pathlib import Path
import re
import shutil
import subprocess


def test_internal_nand_reader(tmp_path):
    root = Path(__file__).resolve().parents[1]
    components = root / "firmware/vibe-mode/components"
    source = components / "app_nand_update/src/factory_nand_update.c"
    fixture = root / "tests/host_bridge"
    cjson = next((root / "firmware/vibe-mode/managed_components/espressif__cjson").rglob("cJSON.h"))
    actual = {p.name for p in components.glob("*/include/*.h")} | {"cJSON.h", "esp_iris_system_update.h"}
    for name in re.findall(r'#include "([^"]+)"', source.read_text()) + ["esp_err.h", "esp_partition.h"]:
        if name not in actual:
            header = tmp_path / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "backend_sdk.h"\n')
    # Product boot header is intentionally absent: unused task code is discarded.
    (tmp_path / "mosaico_boot.h").write_text('int mosaico_boot_request_recovery(void);\n')
    exe = tmp_path / "nand-reader"
    command = [shutil.which("cc"), "-std=gnu11", "-Werror=implicit-function-declaration",
               "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections", "-DBACKEND_TEST",
               f'-DNAND_SOURCE="{source}"', "-I" + str(tmp_path), "-I" + str(fixture),
               "-I" + str(cjson.parent), *["-I" + str(p) for p in components.glob("*/include")],
               "-I" + str(root.parent / "ESP-Iris/components/esp_iris/include"),
               str(fixture / "nand.c"), "-Wl,--gc-sections", "-lcrypto", "-o", str(exe)]
    result = subprocess.run(command, text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
