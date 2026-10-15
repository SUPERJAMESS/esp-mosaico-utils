"""Exercise shared acquisition without either consumer's source tree."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from unittest.mock import patch
import zipfile

import pytest

from shared.gsp import fetch

SHARED = Path(__file__).resolve().parents[2]


def test_windows_release_extracts_and_reuses_exe(tmp_path):
    archive = tmp_path / "gspc-0.3.0-windows-x86_64.zip"
    with zipfile.ZipFile(archive, "w") as bundle:
        bundle.writestr("release/gspc.exe", b"windows executable")
    with patch.object(fetch, "host_os_arch", return_value=("windows", "x86_64")), \
            patch.object(fetch, "download") as download, \
            patch.dict(fetch.os.environ, {"GSPC_EXECUTABLE": ""}):
        executable = fetch.resolve_release(product="gspc", version="0.3.0", env_var="GSPC_EXECUTABLE", cache_dir=tmp_path)
        assert executable == tmp_path / "gspc.exe"
        assert executable.read_bytes() == b"windows executable"
        download.reset_mock()
        assert fetch.resolve_release(product="gspc", version="0.3.0", env_var="GSPC_EXECUTABLE", cache_dir=tmp_path) == executable
        download.assert_not_called()



@pytest.mark.parametrize("product,version", [("gspc", "0.5.0"), ("sim", "1.4.0")])
def test_explicit_release_runs_from_standalone_shared_copy(tmp_path, product, version):
    standalone = tmp_path / "shared modules"
    shutil.copytree(SHARED, standalone, ignore=shutil.ignore_patterns("__pycache__"))
    cache = tmp_path / "cache"
    cache.mkdir()
    executable = cache / (product + (".exe" if os.name == "nt" else ""))
    executable.write_bytes(b"cached tool")
    env = dict(os.environ)
    for key in ("GSPC_EXECUTABLE", "GSP_SIM_EXECUTABLE", "PYTHONPATH"):
        env.pop(key, None)
    command = [sys.executable, "-S", str(standalone / "gsp/fetch.py"),
               "--tool-version", version, "--output-dir", str(cache)]
    if product == "sim":
        command.append("--sim")
    result = subprocess.run(command, cwd=tmp_path, env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    assert Path(result.stdout.strip()) == executable


def test_caller_supplies_pins_and_shared_cli_has_no_default(tmp_path):
    pins = tmp_path / "versions.json"
    pins.write_text(json.dumps({"gspc": "0.5.0", "sim": "1.4.0"}))
    assert fetch.toolchain_version(pins, "gspc") == "0.5.0"
    assert fetch.toolchain_version(pins, "sim") == "1.4.0"
    with pytest.raises(ValueError, match="Missing or invalid"):
        fetch.toolchain_version(pins, "unknown")
    with patch.object(sys, "argv", ["fetch.py"]), pytest.raises(SystemExit) as error:
        fetch.main()
    assert error.value.code == 2


@pytest.mark.parametrize("mode", ["version", "toolchain", "override", "missing"])
def test_cmake_bootstrap_is_independent_and_preserves_selection(tmp_path, mode):
    cmake = shutil.which("cmake")
    if not cmake:
        pytest.skip("CMake unavailable")
    standalone = tmp_path / "standalone shared"
    shutil.copytree(SHARED / "gsp", standalone)
    pins = tmp_path / "versions.json"
    pins.write_text('{"gspc": "0.6.1"}')
    for version in ("0.5.0", "0.6.1"):
        directory = tmp_path / "cache/esp-mosaico/gspc" / version
        directory.mkdir(parents=True)
        (directory / ("gspc.exe" if os.name == "nt" else "gspc")).write_bytes(b"cached")
    result_file = tmp_path / "resolved.txt"
    lines = ['set(MOSAICO_GSPC_PYTHON "' + Path(sys.executable).as_posix() + '")']
    selected = "0.5.0"
    if mode == "version":
        lines.append('set(MOSAICO_GSPC_VERSION "0.5.0")')
    elif mode == "toolchain":
        lines.append('set(MOSAICO_GSP_TOOLCHAIN "' + pins.as_posix() + '")')
        selected = "0.6.1"
    lines.extend(['include("' + (standalone / "compiler.cmake").as_posix() + '")',
                  'file(WRITE "' + result_file.as_posix() + '" "${GSPC_EXECUTABLE}")'])
    script = tmp_path / "resolve.cmake"
    script.write_text("\n".join(lines))
    env = dict(os.environ, XDG_CACHE_HOME=str(tmp_path / "cache"))
    for key in ("GSPC_EXECUTABLE", "GSPC_CACHE_DIR", "PYTHONPATH"):
        env.pop(key, None)
    expected = tmp_path / "cache/esp-mosaico/gspc" / selected / ("gspc.exe" if os.name == "nt" else "gspc")
    if mode == "override":
        env["GSPC_EXECUTABLE"] = expected.as_posix()
    result = subprocess.run([cmake, "-P", str(script)], cwd=tmp_path, env=env,
                            capture_output=True, text=True, timeout=20)
    if mode == "missing":
        assert result.returncode != 0
        assert "Set MOSAICO_GSPC_VERSION" in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        assert Path(result_file.read_text()) == expected
