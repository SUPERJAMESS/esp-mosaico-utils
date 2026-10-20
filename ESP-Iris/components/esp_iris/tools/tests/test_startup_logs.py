"""Exercise the production early stdio tap and bounded log ring on the host."""
import re
import subprocess
import sys
from pathlib import Path

import pytest


@pytest.mark.skipif(sys.platform != "linux", reason="The VFS harness uses GNU fopencookie")
def test_startup_capture_and_overflow(tmp_path):
    tests = Path(__file__).resolve().parent
    sources = tests.parents[1] / "src"
    for name in ("esp_iris_log.c", "esp_iris_startup.c"):
        source = (sources / name).read_text()
        for header_name in re.findall(r'#include ["<]([^">]+)[">]', source):
            if header_name.startswith("esp_") or header_name == "sys/lock.h":
                header = tmp_path / header_name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "sdk.h"\n')
        (tmp_path / name).write_text(source)
    output = tmp_path / "startup-logs"
    build = subprocess.run([
        "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-I" + str(tmp_path),
        "-I" + str(tests / "log_host"), str(tests / "log_host/main.c"),
        "-o", str(output),
    ], capture_output=True, text=True, timeout=30, check=False)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=10, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
