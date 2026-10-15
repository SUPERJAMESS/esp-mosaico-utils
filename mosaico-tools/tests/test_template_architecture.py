"""Guard application boundaries and run public C contracts without a consumer workspace."""

import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import pytest

TOOLS = Path(__file__).resolve().parents[1]
TEMPLATES = TOOLS / "templates"
FIXTURES = Path(__file__).parent / "fixtures/template_contracts"


@pytest.mark.parametrize("template", ["hello_world", "blank_game"])
def test_builtin_component_boundaries_and_dependency_graph(template):
    root = TEMPLATES / template
    components = {path.name: path for path in (root / "components").iterdir() if path.is_dir()}
    graph = {}
    for name, directory in {"main": root / "main", **components}.items():
        assert name == "main" or name.startswith("app_")
        assert (directory / "README.md").is_file()
        text = (directory / "CMakeLists.txt").read_text()
        args = shlex.split(re.search(r"idf_component_register\((.*?)\)", text, re.S)[1])
        group = None
        graph[name] = []
        for value in args:
            if value in {"SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES", "PRIV_REQUIRES"}:
                group = value
            elif group == "INCLUDE_DIRS":
                assert value == "include", (name, value)
            elif group in {"REQUIRES", "PRIV_REQUIRES"} and value in {"main", *components}:
                graph[name].append(value)
        if name != "main":
            assert list((directory / "include").glob("*.h"))
        for source in directory.rglob("*.[ch]"):
            text = source.read_text()
            assert not re.search(r"\bextern\s+(?!\"C\")", text), source
            for included in re.findall(r'#include "([^"]+)"', text):
                target = (source.parent / included).resolve()
                if (root / "components").resolve() in target.parents and directory.resolve() not in target.parents:
                    assert "include" in target.relative_to(root / "components").parts, (source, included)
                    assert "private" not in target.parts
    def visit(name, ancestors):
        assert name not in ancestors, f"Component cycle: {ancestors + [name]}"
        for dependency in graph[name]:
            assert dependency != "main", f"{name} depends back on composition root"
            visit(dependency, ancestors + [name])
    for name in graph:
        visit(name, [])
    policy = json.loads((root / "tools/quality/policy.json").read_text())
    assert policy["application_component_prefix"] == "app_"


@pytest.mark.parametrize("template,component,fixture,stub_gsp", [
    ("blank_game", "app_game", "game_contract.c", False),
    ("hello_world", "app_hello", "hello_contract.c", True),
])
def test_public_component_behavior(tmp_path, template, component, fixture, stub_gsp):
    compiler = next((shutil.which(name) for name in ("cc", "gcc", "clang") if shutil.which(name)), None)
    if not compiler:
        if os.environ.get("IRIS_REQUIRE_HOST_CC") == "1":
            pytest.fail("A native C compiler is required")
        pytest.skip("No native C compiler")
    directory = TEMPLATES / template / "components" / component
    executable = tmp_path / ("contract.exe" if os.name == "nt" else "contract")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(directory / "include")]
    if stub_gsp:
        command.extend(["-I", str(FIXTURES)])
    command.extend([str(directory / (component + ".c")), str(FIXTURES / fixture), "-o", str(executable)])
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(executable)], check=True, timeout=10)


@pytest.mark.parametrize("te_enabled", [0, 1])
def test_device_display_returns_co5300_alignment(tmp_path, te_enabled):
    """Check the device config contract, not physical panel rendering."""
    compiler = next((shutil.which(name) for name in ("cc", "gcc", "clang") if shutil.which(name)), None)
    if not compiler:
        if os.environ.get("IRIS_REQUIRE_HOST_CC") == "1":
            pytest.fail("A native C compiler is required")
        pytest.skip("No native C compiler")
    # The harness supplies SDK types and hardware stubs. Compile the real device
    # adapter and public header, independently of managed components/workspaces.
    for name in ("esp_err.h", "esp_gsp_esp_lcd.h", "bsp/esp_mosaico.h",
                 "esp_check.h", "esp_log.h", "sdkconfig.h"):
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.touch()
    directory = TEMPLATES / "hello_world/components/app_display"
    executable = tmp_path / ("display.exe" if os.name == "nt" else "display")
    result = subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         f"-DCONFIG_BSP_CO5300_ENABLE_TE={te_enabled}",
         "-I", str(tmp_path), "-I", str(directory), "-I", str(directory / "include"),
         str(FIXTURES / "display_contract.c"), "-o", str(executable)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(executable)], check=True, timeout=10)
