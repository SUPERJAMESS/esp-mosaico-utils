from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/normalize_gsp_depfile.py"
SPEC = importlib.util.spec_from_file_location("gsp_depfile", SCRIPT)
depfile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(depfile)


def test_normalize_extended_windows_paths_and_escaped_assets():
    text = r"\\\\?\\D:\\work\ space\\bundle.gspb: \\\\?\\D:\\work\ space\\font\#1.ttf" + "\n"
    assert depfile.normalize(text) == "D:/work\\ space/bundle.gspb: D:/work\\ space/font\\#1.ttf\n"


def test_normalizer_preserves_unaffected_depfiles(tmp_path):
    path = tmp_path / "bundle.d"
    original = b"/tmp/bundle.gspb: /tmp/a\\ b.ttf\n"
    path.write_bytes(original)
    depfile.normalize_file(path)
    assert path.read_bytes() == original


@pytest.mark.skipif(os.name != "nt", reason="Windows canonical-path regression")
@pytest.mark.parametrize("existing_bad_dependencies", [False, True])
def test_cmake_ninja_incremental_depfile_tracks_assets(tmp_path, existing_bad_dependencies):
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    if not cmake or not ninja:
        pytest.skip("CMake and Ninja required")
    source = tmp_path / "project space"
    source.mkdir()
    asset = source / "font 1.ttf"
    asset.write_bytes(b"first")
    builder = source / "build_bundle.py"
    builder.write_text(
        'from pathlib import Path\n'
        'import sys\n'
        'asset, output, dep = map(Path, sys.argv[1:])\n'
        'output.write_bytes(asset.read_bytes())\n'
        'def escaped(p):\n'
        '    value = "\\\\\\\\?\\\\" + str(p.resolve())\n'
        '    return value.replace("\\\\", "\\\\\\\\").replace(" ", "\\\\ ").replace("#", "\\\\#")\n'
        'dep.write_text(escaped(output) + ": " + escaped(asset) + "\\n", encoding="utf-8")\n',
        encoding="utf-8",
    )
    # APPEND is how mosaico_gsp_add_ui_bundle attaches normalization to the
    # upstream command, before CMake transforms the DEPFILE for Ninja.
    cmake_file = source / "CMakeLists.txt"
    cmake_file.write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(depfile NONE)\n'
        'set(bundle "${CMAKE_BINARY_DIR}/bundle.gspb")\n'
        'set(dep "${CMAKE_BINARY_DIR}/bundle.d")\n'
        'if(EXISTS "${dep}")\n'
        f'execute_process(COMMAND "{Path(sys.executable).as_posix()}" "{SCRIPT.as_posix()}" '
        '"${dep}" --repair-ninja-cache "${CMAKE_BINARY_DIR}" COMMAND_ERROR_IS_FATAL ANY)\nendif()\n'
        f'add_custom_command(OUTPUT "${{bundle}}" COMMAND "{Path(sys.executable).as_posix()}" '
        f'"{builder.as_posix()}" "{asset.as_posix()}" "${{bundle}}" "${{dep}}" DEPFILE "${{dep}}" VERBATIM)\n'
        f'add_custom_command(OUTPUT "${{bundle}}" APPEND COMMAND "{Path(sys.executable).as_posix()}" '
        f'"{SCRIPT.as_posix()}" "${{dep}}" VERBATIM)\n'
        'add_custom_target(bundle ALL DEPENDS "${bundle}")\n', encoding="utf-8",
    )
    build = tmp_path / "build space"

    def run(*args):
        return subprocess.run([cmake, *args], capture_output=True, text=True, check=True, timeout=30)

    run("-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja)
    if existing_bad_dependencies:
        fixed = cmake_file.read_text(encoding="utf-8")
        cmake_file.write_text("\n".join(line for line in fixed.splitlines() if not line.startswith("add_custom_command(OUTPUT")
                                       or "APPEND" not in line), encoding="utf-8")
        run("-S", str(source), "-B", str(build))
        run("--build", str(build))
        cmake_file.write_text(fixed, encoding="utf-8")
        run("-S", str(source), "-B", str(build))
    run("--build", str(build))
    before = (build / "bundle.gspb").stat().st_mtime_ns
    assert "no work to do" in run("--build", str(build)).stdout
    assert (build / "bundle.gspb").stat().st_mtime_ns == before
    asset.write_bytes(b"updated")
    run("--build", str(build))
    assert (build / "bundle.gspb").read_bytes() == b"updated"
    assert "no work to do" in run("--build", str(build)).stdout
