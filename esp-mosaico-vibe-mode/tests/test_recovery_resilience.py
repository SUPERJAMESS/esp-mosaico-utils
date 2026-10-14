"""Run Recovery service failure paths against deterministic ESP-IDF fakes."""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ROOT / "firmware/vibe-mode/components"
FIXTURE = ROOT / "tests/host_resilience"


@pytest.mark.parametrize("kind,source", [
    ("ui", "factory_ui_dispatch.c"),
    ("network", "factory_network.c"),
    ("ota", "recovery_ota_support.c"),
])
def test_recovery_resilience(tmp_path, kind, source):
    cc = shutil.which("cc")
    assert cc, "Recovery service regression tests require a C compiler"
    component = COMPONENTS / {"ui": "app_ui", "network": "app_network", "ota": "app_iris"}[kind]
    implementation = component / "src" / source
    headers = re.findall(r'#include "([^"]+)"', implementation.read_text())
    headers += ["esp_err.h", "esp_gsp.h", "esp_iris_system_inventory.h", "esp_iris_system_update.h", "cJSON.h"]
    actual_headers = {"esp_iris_system_update.h", "esp_iris_system_inventory.h"} | {path.name for path in COMPONENTS.glob("*/include/*.h")} | {
        path.name for path in (component / "private").glob("*.h")}
    for name in headers:
        if name in actual_headers:
            continue
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    output = tmp_path / ("resilience.exe" if os.name == "nt" else "resilience")
    command = [cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", f'-DSOURCE="{implementation.as_posix()}"',
               "-I" + str(tmp_path), "-I" + str(FIXTURE), *["-I" + str(path) for path in COMPONENTS.glob("*/include")],
               "-I" + str(component / "private"),
               "-I" + str(ROOT / "include"), "-I" + str(ROOT.parent / "ESP-Iris/components/esp_iris/include"), str(FIXTURE / (kind + ".c")),
               str(FIXTURE / "sdk.c"), "-o", str(output)]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    build = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=15, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
