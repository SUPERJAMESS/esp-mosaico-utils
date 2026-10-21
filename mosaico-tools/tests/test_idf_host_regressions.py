"""Regressions from Windows builds and checkouts with incomplete Git tags."""
import importlib.util
import io
import json
import subprocess
import sys
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from mosaico_cli import doctor, host, runtime
from mosaico_cli.cli import _emit_error
from mosaico_cli.errors import BuildError, EnvironmentError
from mosaico_cli.workspace import load_workspace


def test_doctor_decodes_utf8_output_independent_of_windows_codepage():
    code, output = doctor._command([sys.executable, "-c", "print('中文路径')"], environment={})
    assert code == 0 and output == "中文路径"

SPEC = importlib.util.spec_from_file_location(
    "host_regression_runner", ROOT / "skills/idf-low-noise-build/scripts/idf_low_noise_build.py"
)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


@pytest.mark.parametrize("options", [
    ["-B", "build-02", "build"], ["--build-dir", "build-02", "build"],
    ["--build-dir=build-02", "build"], ["-Bbuild-02", "build"],
])
def test_runner_never_reports_old_default_artifact_for_selected_build(tmp_path, options):
    for directory, binary in (("build", b"old reviewed image"),
                              ("build-02", b"new candidate image")):
        build = tmp_path / directory
        build.mkdir()
        (build / "factory.bin").write_bytes(binary)
        (build / "project_description.json").write_text(json.dumps({
            "app_bin": "factory.bin", "target": "esp32s31"}))
    selected = runner.selected_build_dir(tmp_path, options)
    assert runner.collect_artifacts(tmp_path, selected) == [{
        "path": str(tmp_path / "build-02/factory.bin"),
        "size_bytes": len(b"new candidate image"),
    }]


@pytest.mark.parametrize("declaration,expected", [
    ('  idf: ">=5.5"\n', ">=5.5"),
    ('  idf:\n    version: ">=5.5, <7.0"\n', ">=5.5, <7.0"),
    ('  idf: # SDK\n    # supported SDK\n    version: ">=5.5"\n', ">=5.5"),
    ('  idf:\n  other:\n    version: "1.0"\n', None),
])
def test_runner_reads_inline_and_nested_idf_constraints(tmp_path, declaration, expected):
    main = tmp_path / "main"
    main.mkdir()
    (main / "idf_component.yml").write_text("dependencies:\n" + declaration)
    assert runner.declared_idf_constraint(tmp_path) == expected


def write_version(root, version="6.2.0"):
    path = root / "tools/cmake/version.cmake"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(
        f'set(IDF_VERSION_{name} {value})' for name, value in
        zip(("MAJOR", "MINOR", "PATCH"), version.split("."))
    ), encoding="utf-8")


@pytest.mark.parametrize("platform,policy,inherited,expected", [
    ("windows", "auto", None, "0"),
    ("linux", "auto", None, None),
    ("macos", "auto", None, None),
    ("windows", "auto", "1", "1"),
    ("windows", "off", "1", "0"),
    ("windows", "on", "0", "1"),
    ("linux", "off", None, "0"),
])
def test_configdep_policy(platform, policy, inherited, expected):
    values = {"PATH": "original"}
    if inherited is not None:
        values["IDF_CONFIGDEP_ENABLE"] = inherited
    original = values.copy()
    with patch.object(host, "host_platform", return_value=platform):
        result = host.configdep_environment(policy, environment=values)
    assert result.get("IDF_CONFIGDEP_ENABLE") == expected
    assert result["PATH"] == "original"
    assert values == original


@pytest.mark.parametrize("source,reported,expected", [
    ("6.2.0", "v6.1-dev-999-g7b9cc1ac", True),
    ("6.1.9", "v6.2-dev-999-g12345678", False),
    (None, "v6.1-dev-999-g7b9cc1ac", False),
    (None, "v6.2-dev-999-g7b9cc1ac", True),
])
def test_both_doctors_prefer_source_version(tmp_path, source, reported, expected):
    if source:
        write_version(tmp_path, source)
    manifest = tmp_path / "main/idf_component.yml"
    manifest.parent.mkdir()
    manifest.write_text('dependencies:\n  idf: ">=6.2"\n', encoding="utf-8")
    (tmp_path / "sdkconfig.defaults").write_text('CONFIG_IDF_TARGET="esp32s31"\n')
    prepared = host.IdfEnvironment(tmp_path, Path(sys.executable), tmp_path / "tools/idf.py", {})
    workspace = SimpleNamespace(idf_constraint_manifest=manifest, recovery_project=tmp_path)

    def command(argv, *args, **kwargs):
        if "--list-targets" in argv:
            return 0, "esp32s31\n"
        if "rev-parse" in argv:
            return 0, "7b9cc1ac79f8"
        if "doctor" in argv:
            return 0, '{"python_supported": true, "devices": []}'
        if len(argv) == 2:
            return 0, "Python 3.12.3"
        return 0, "ESP-IDF " + reported

    with patch.object(doctor, "state_root", return_value=tmp_path / "state"), \
         patch.object(doctor, "select_model", return_value=SimpleNamespace(target="esp32s31")), \
         patch.object(doctor, "resolve_idf_path", return_value=tmp_path), \
         patch.object(doctor, "prepare_idf_environment", return_value=prepared), \
         patch.object(doctor, "locate_iris_tools", return_value=(sys.executable, "iris.py")), \
         patch.object(doctor, "_command", side_effect=command):
        result = doctor.diagnose_host(workspace)
    assert (result["status"] == "ready") is expected
    assert reported in result["idf"]["reported_version"]
    assert result["idf"]["version"] == (source or "ESP-IDF " + reported)
    assert result["idf"]["version_source"] == ("tools/cmake/version.cmake" if source else "idf.py --version")
    output = io.StringIO()
    with patch.object(runner, "idf_command", return_value=([sys.executable, "idf.py", "--version"], {}, Path(sys.executable))), \
         patch.object(runner, "command_output", side_effect=command), redirect_stdout(output):
        code = runner.doctor(tmp_path, tmp_path)
    assert (code == 0) is expected
    assert f"idf_version: {source or 'ESP-IDF ' + reported}" in output.getvalue()


def test_partial_source_version_falls_back(tmp_path):
    write_version(tmp_path)
    path = tmp_path / "tools/cmake/version.cmake"
    path.write_text('set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 2)\n')
    assert host.idf_source_version(tmp_path) is None


CONFIGDEP_FAILURE = (
    "[900/1000] Building object\nFAILED: lvgl_bridge_v9.c.obj\n"
    "utils.c:64: in touch_file: cannot create\n"
    "'C:/app/build/config/esp/lvgl/adapter/partial/aux/img/cache.cdep':\n"
    "No such file or directory (2)\nninja: build stopped: subcommand failed.\n"
)


@pytest.mark.parametrize("output,category,detail", [
    (CONFIGDEP_FAILURE, "configdep", "aux/img/cache.cdep"),
    ("FAILED: bundle\npacker: unknown input format\n", "ninja", "unknown input format"),
    ("FAILED: file.obj\nmain.c:5: error: undeclared identifier\n", "compiler", "undeclared identifier"),
])
def test_failure_surfaces_real_stdout_on_stderr(output, category, detail):
    prepared = host.IdfEnvironment(Path("/idf"), Path(sys.executable), Path("/idf/tools/idf.py"), {})
    context = Mock(workspace=SimpleNamespace(root=Path.cwd(), configdep="auto"), log_path=Path("raw.log"))
    context.run.return_value = subprocess.CompletedProcess([], 1, output)
    with patch.object(runtime, "prepare_idf_environment", return_value=prepared), pytest.raises(BuildError) as failure:
        runtime.run_idf_target(context, idf_path=Path("/idf"), project=Path("/app"),
                               build_dir=Path("/app/build"), target="system-update-bundle", timeout=30)
    error_output = io.StringIO()
    with redirect_stderr(error_output):
        _emit_error(failure.value, False, False)
    assert detail in error_output.getvalue()
    assert f"({category})" in error_output.getvalue()
    json_output = io.StringIO()
    with redirect_stderr(json_output):
        _emit_error(failure.value, True, False)
    assert detail in json.loads(json_output.getvalue())["details"]["diagnostic"]
    diagnostic = runner.find_diagnostic(output)
    assert diagnostic["category"] == category
    assert detail in diagnostic["excerpt"]


def test_no_error_diagnostic_for_success():
    assert runtime._idf_failure_diagnostic("Project build complete.\n") is None
    assert runner.find_diagnostic("Project build complete.\n") is None


def test_configdep_failure_excerpt_is_bounded():
    output = CONFIGDEP_FAILURE + "unrelated later output\n" * 100
    assert len(runtime._idf_failure_diagnostic(output).splitlines()) <= 21
    assert len(runner.find_diagnostic(output)["excerpt"].splitlines()) <= 29


def test_windows_environment_preparation_disables_configdep(tmp_path):
    idf = tmp_path / "idf"
    (idf / "tools").mkdir(parents=True)
    for name in ("idf.py", "idf_tools.py"):
        (idf / "tools" / name).touch()
    python = host.virtual_environment_python(tmp_path / "python env")
    python.parent.mkdir(parents=True)
    python.touch()
    inherited = {"PATH": "original", "IDF_PYTHON_ENV_PATH": str(tmp_path / "python env")}
    with patch.object(host, "host_platform", return_value="windows"), \
         patch.object(host, "_probe_python", return_value=(python, (3, 12))), \
         patch.object(host.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "PATH=idf-tools\n")):
        prepared = host.prepare_idf_environment(idf, base_environment=inherited, bootstrap_python=python)
    assert prepared.values["IDF_CONFIGDEP_ENABLE"] == "0"
    assert "IDF_CONFIGDEP_ENABLE" not in inherited


@pytest.mark.parametrize("policy", ["on", "off"])
def test_workspace_policy_reaches_both_build_entrypoints(tmp_path, policy):
    runner_file = tmp_path / "runner.py"
    runner_file.touch()
    workspace = SimpleNamespace(root=tmp_path, configdep=policy, build_runner=runner_file)
    context = Mock(workspace=workspace, directory=tmp_path, log_path=tmp_path / "raw.log")
    context.run.return_value = subprocess.CompletedProcess([], 0, "")
    with patch.object(runtime, "resolve_idf_path", return_value=tmp_path):
        runtime.build_application(context, tmp_path)
    assert context.run.call_args.kwargs["env"]["IDF_CONFIGDEP_ENABLE"] == ("1" if policy == "on" else "0")
    prepared = host.IdfEnvironment(tmp_path, Path(sys.executable), tmp_path / "idf.py", {})
    with patch.object(runtime, "prepare_idf_environment", return_value=prepared):
        command = runtime.idf_target_command(context, idf_path=tmp_path, project=tmp_path,
                    build_dir=tmp_path / "build", target="system-update-bundle", timeout=30)
    assert command["env"]["IDF_CONFIGDEP_ENABLE"] == ("1" if policy == "on" else "0")


@pytest.mark.parametrize("policy", [True, "invalid", [], {}])
def test_invalid_workspace_policy_has_actionable_error(tmp_path, policy):
    (tmp_path / ".mosaico.json").write_text(json.dumps({
        "schema_version": 2, "workspace": {}, "dependencies": {},
        "build": {"configdep": policy}, "devices": [{}],
    }))
    with pytest.raises(EnvironmentError, match="build.configdep must be"):
        load_workspace(ROOT, explicit=str(tmp_path))
