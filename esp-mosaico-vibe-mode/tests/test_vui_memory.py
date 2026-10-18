"""Exercise renderer ownership and asynchronous input with optional ASan/UBSan."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_renderer_resource_lifetime_and_repeated_input(tmp_path):
    configure = [
        "cmake",
        "-S",
        str(ROOT / "firmware/vibe-mode/pc"),
        "-B",
        str(tmp_path),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DPython3_EXECUTABLE=" + sys.executable,
        "-DVUI_TEST_MAIN=" + (ROOT / "tests/host_vui/runtime.c").as_posix(),
    ]
    if os.name == "nt":
        configure += ["-G", "MinGW Makefiles"]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        configure += [
            "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O2",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
        ]
    for command in (
        configure,
        ["cmake", "--build", str(tmp_path), "--config", "Debug", "-j4"],
    ):
        result = subprocess.run(
            command, capture_output=True, text=True, check=False, timeout=120
        )
        assert result.returncode == 0, result.stdout + result.stderr
    executable = tmp_path / (
        "vui_runtime_test.exe" if os.name == "nt" else "vui_runtime_test"
    )
    result = subprocess.run(
        [str(executable)], capture_output=True, text=True, check=False, timeout=120
    )
    assert result.returncode == 0, result.stdout + result.stderr
