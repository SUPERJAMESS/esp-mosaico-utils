"""Workspace-owned creation, project selection and actionable host diagnostics.

Use temporary workspace/template descriptions and the mainline CLI contracts.
Applications share the workspace configuration and launcher; project creation
does not package an independent SDK or return an agent handoff.
"""

import json
import os
from pathlib import Path
import shlex
import sys
from unittest.mock import Mock

import pytest

TOOL_ROOT = Path(__file__).resolve().parents[2] / "mosaico-tools"
sys.path.insert(0, str(TOOL_ROOT / "tools"))

from mosaico_cli import cli, doctor
from mosaico_cli.errors import EnvironmentError
from mosaico_cli.project import resolve_project
from mosaico_cli.scaffold import initialize_project
from mosaico_cli.workspace import load_workspace


@pytest.fixture(autouse=True)
def isolate_cli_environment(monkeypatch):
    monkeypatch.setattr(cli.MosaicoArgumentParser, "json_errors", False)
    monkeypatch.delenv("MOSAICO_WORKSPACE", raising=False)


@pytest.fixture
def make_workspace(tmp_path):
    def create(name="workspace with spaces 中文"):
        root = (tmp_path / name).resolve()
        template = root / "templates/basic"
        template.mkdir(parents=True)
        config = {
            "schema_version": 2,
            "workspace": {
                "projects_dir": "apps with spaces/nested",
                "init_template": "templates/basic/description.json",
            },
            "dependencies": {"bsp": "vendor/board", "esp_iris": "vendor/iris"},
            "build": {"runner": "builtin"},
            "devices": [{"id": "test-s31", "name": "Test S31", "target": "esp32s31",
                         "status": "supported", "default": True,
                         "preview_target": True, "recovery_usb_ids": []}],
        }
        (root / ".mosaico.json").write_text(json.dumps(config), encoding="utf-8")
        (template / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(template_app)\n", encoding="utf-8")
        (template / "entry.c").write_text("void app_main(void) {}\n", encoding="utf-8")
        (template / "description.json").write_text(json.dumps({
            "schema_version": 1,
            "files": [
                {"source": "CMakeLists.txt", "replacements": [
                    {"pattern": "template_app", "replacement": "{{project_name}}"},
                ]},
                {"source": "entry.c", "destination": "main/main.c"},
            ],
        }), encoding="utf-8")
        return load_workspace(TOOL_ROOT, explicit=str(root))

    return create


@pytest.fixture
def diagnose(monkeypatch):
    probe = Mock(return_value={
        "command": "doctor", "status": "ready", "host": {}, "idf": {}, "iris": {},
        "checks": [], "exit_code": 0,
    })
    monkeypatch.setattr(cli, "diagnose_host", probe)
    return probe


@pytest.mark.parametrize("location", ["workspace", "project", "nested"])
def test_doctor_discovers_workspace_from_application(
    make_workspace, monkeypatch, capsys, diagnose, location
):
    workspace = make_workspace()
    project = Path(initialize_project(workspace, "demo")["project"])
    directory = {"workspace": workspace.root, "project": project, "nested": project / "main"}[location]
    monkeypatch.chdir(directory)

    assert cli.main(["doctor", "--json"]) == 0

    assert json.loads(capsys.readouterr().out)["ok"] is True
    diagnose.assert_called_once_with(workspace)
    assert Path.cwd() == directory


@pytest.mark.parametrize("selection", ["directory", "config"])
@pytest.mark.parametrize("style", ["separate", "equals"])
def test_explicit_workspace_overrides_cwd_and_environment(
    make_workspace, monkeypatch, capsys, diagnose, selection, style
):
    current = make_workspace("current")
    environment = make_workspace("environment")
    selected = make_workspace("selected with spaces 中文")
    monkeypatch.chdir(current.root)
    monkeypatch.setenv("MOSAICO_WORKSPACE", str(environment.root))
    path = selected.root if selection == "directory" else selected.config_path
    relative = os.path.relpath(path, current.root)
    option = ["--workspace", relative] if style == "separate" else ["--workspace=" + relative]

    assert cli.main(["doctor", *option, "--json"]) == 0

    assert json.loads(capsys.readouterr().out)["status"] == "ready"
    diagnose.assert_called_once_with(selected)
    assert Path.cwd() == current.root


def test_environment_workspace_overrides_cwd(make_workspace, monkeypatch, capsys, diagnose):
    current = make_workspace("current")
    selected = make_workspace("selected")
    monkeypatch.chdir(current.root)
    monkeypatch.setenv("MOSAICO_WORKSPACE", str(selected.config_path))

    assert cli.main(["doctor", "--json"]) == 0

    assert json.loads(capsys.readouterr().out)["ok"] is True
    diagnose.assert_called_once_with(selected)


@pytest.mark.parametrize("location", ["workspace", "existing_project", "outside"])
def test_init_creates_in_selected_workspace_and_returns_usable_install_command(
    make_workspace, tmp_path, monkeypatch, capsys, location
):
    workspace = make_workspace()
    existing = Path(initialize_project(workspace, "existing")["project"])
    directory = {"workspace": workspace.root, "existing_project": existing / "main",
                 "outside": tmp_path}[location]
    monkeypatch.chdir(directory)
    before = workspace.config_path.read_bytes()
    # Creation must work without starting a Gateway, build or device session.
    monkeypatch.setattr(cli, "RunContext", Mock(side_effect=AssertionError("runtime started")))

    assert cli.main(["project", "init", "demo", "--workspace", str(workspace.root), "--json"]) == 0

    report = json.loads(capsys.readouterr().out)
    project = workspace.projects_dir / "demo"
    assert report["ok"] and report["status"] == "created"
    assert Path(report["project"]) == project
    assert Path(report["template"]) == workspace.init_template
    assert {"CMakeLists.txt", "main/main.c", ".pre-commit-config.yaml",
            "tools/quality/policy.json"} <= set(report["files"])
    assert report["git"] == {"status": "initialized", "branch": "main"}
    assert (project / ".git").is_dir()
    assert all((project / name).is_file() for name in report["files"])
    assert "project(demo)" in (project / "CMakeLists.txt").read_text(encoding="utf-8")
    assert workspace.config_path.read_bytes() == before
    assert not (project / ".mosaico.json").exists()
    assert not (project / "mosaico.py").exists()
    assert not workspace.run_dir.exists()
    assert Path.cwd() == directory

    # The reported command is for the workspace launcher. Validate its argument
    # parsing and selected application without invoking its device operation.
    command = shlex.split(report["install_command"])
    assert command[:2] == ["python", "mosaico.py"]
    arguments = cli.build_parser().parse_args(command[2:])
    assert arguments.command == "system-update"
    assert resolve_project(workspace, arguments.project, workspace.root).samefile(project)
    assert resolve_project(workspace, None, existing / "main").samefile(existing)


def test_init_dry_run_explains_workspace_followup_without_writing(make_workspace, monkeypatch, capsys):
    workspace = make_workspace()
    monkeypatch.chdir(workspace.root)
    before = workspace.config_path.read_bytes()
    monkeypatch.setattr(cli, "RunContext", Mock(side_effect=AssertionError("runtime started")))

    assert cli.main(["project", "init", "demo", "--dry-run"]) == 0

    output = capsys.readouterr().out
    assert "init: dry_run" in output
    assert "Checks passed; no files were written." in output
    assert "From workspace: " + str(workspace.root) in output
    assert "python mosaico.py iris system-update --project " in output
    assert not workspace.projects_dir.exists()
    assert workspace.config_path.read_bytes() == before


@pytest.mark.parametrize("content", [
    "{invalid", "[]", '{"schema_version":1}', '{"schema_version":2,"unknown":true}',
])
@pytest.mark.parametrize("command", [["doctor"], ["project", "init", "demo"]])
def test_invalid_nearest_config_fails_before_work(
    make_workspace, monkeypatch, capsys, content, command
):
    workspace = make_workspace()
    nested = workspace.root / "nested"
    nested.mkdir()
    (nested / ".mosaico.json").write_text(content, encoding="utf-8")
    monkeypatch.chdir(nested)
    diagnose = Mock(side_effect=AssertionError("invalid workspace reached diagnostics"))
    initialize = Mock(side_effect=AssertionError("invalid workspace reached creation"))
    monkeypatch.setattr(cli, "diagnose_host", diagnose)
    monkeypatch.setattr(cli, "initialize_project", initialize)

    assert cli.main([*command, "--json"]) == 3

    captured = capsys.readouterr()
    report = json.loads(captured.err)
    assert not captured.out
    assert report["ok"] is False and report["error"] == "environment_error"
    assert report["exit_code"] == 3 and report["message"]
    diagnose.assert_not_called()
    initialize.assert_not_called()
    assert not workspace.projects_dir.exists()


@pytest.mark.parametrize("kind", ["missing", "directory", "dangling"])
def test_explicit_invalid_config_does_not_fall_back_to_parent(
    make_workspace, monkeypatch, capsys, diagnose, kind
):
    workspace = make_workspace()
    child = workspace.root / "child"
    child.mkdir()
    path = child / ".mosaico.json"
    if kind == "directory":
        path.mkdir()
    elif kind == "dangling":
        try:
            path.symlink_to(child / "absent.json")
        except OSError:
            pytest.skip("Symlinks unavailable on this host")
    monkeypatch.chdir(child)

    assert cli.main(["doctor", "--workspace", str(path), "--json"]) == 3

    report = json.loads(capsys.readouterr().err)
    assert report["error"] == "environment_error" and str(child) in report["message"]
    diagnose.assert_not_called()


@pytest.mark.parametrize("iris_ready", [True, False])
@pytest.mark.parametrize("json_output", [True, False])
def test_doctor_reports_missing_dependencies_with_mainline_schema(
    make_workspace, tmp_path, monkeypatch, capsys, iris_ready, json_output
):
    workspace = make_workspace()
    monkeypatch.chdir(workspace.root)
    monkeypatch.setattr(doctor, "state_root", lambda _: tmp_path / "state")
    monkeypatch.setattr(doctor, "host_platform", lambda: "linux")
    monkeypatch.setattr(doctor, "resolve_idf_path", Mock(side_effect=EnvironmentError("IDF not configured")))
    # Only external probes are replaced: run the real doctor and CLI reporting.
    locate = Mock(return_value=(sys.executable, tmp_path / "iris.py")) if iris_ready else Mock(
        side_effect=EnvironmentError("Gateway not prepared"))
    monkeypatch.setattr(doctor, "locate_iris_tools", locate)
    command = Mock(return_value=(0, json.dumps({"python_supported": True, "devices": []})))
    monkeypatch.setattr(doctor, "_command", command)

    assert cli.main(["doctor", *(["--json"] if json_output else [])]) == 3

    captured = capsys.readouterr()
    assert not captured.err
    if json_output:
        report = json.loads(captured.out)
        assert report["command"] == "doctor" and report["status"] == "not_ready"
        assert report["ok"] is False and report["exit_code"] == 3
        assert report["host"]["python_supported"] is True
        assert report["host"]["python"] == ".".join(str(part) for part in sys.version_info[:3])
        assert report["idf"]["error"] == "IDF not configured"
        checks = {check["name"]: check for check in report["checks"]}
        assert checks["idf-environment"]["status"] == "fail"
        assert checks["esp-iris"]["status"] == ("pass" if iris_ready else "fail")
        if iris_ready:
            assert checks["usb"]["status"] == "warn" and report["iris"]["devices"] == []
        else:
            assert report["iris"]["error"] == "Gateway not prepared"
    else:
        assert "MOSAICO HOST DOCTOR" in captured.out
        assert "IDF not configured" in captured.out
        assert "status: not_ready" in captured.out
        assert ("No compatible USB device" if iris_ready else "Gateway not prepared") in captured.out
    locate.assert_called_once_with(workspace)
    if iris_ready:
        command.assert_called_once_with([sys.executable, str(tmp_path / "iris.py"), "doctor", "--json"])
    else:
        command.assert_not_called()
    assert not workspace.run_dir.exists()
