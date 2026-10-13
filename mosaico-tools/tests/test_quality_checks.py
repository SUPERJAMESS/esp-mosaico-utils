"""Exercise the shared checker against isolated application repositories."""

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

TOOLS = Path(__file__).resolve().parents[1]


def run_git(project, *arguments):
    return subprocess.run(["git", "-C", str(project), *arguments], capture_output=True, text=True, check=True)


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "application"
    root.mkdir()
    run_git(root, "init")
    policy = root / "tools/quality/policy.json"
    policy.parent.mkdir(parents=True)
    policy.write_text("{}\n")
    return root


def run_check(project, check, *arguments, env=None):
    return subprocess.run(
        [sys.executable, str(TOOLS / "tools/quality/check_project.py"), check, *arguments],
        cwd=project,
        env=env,
        capture_output=True,
        text=True,
    )


def test_artifacts_do_not_reject_source_assets(project):
    bad = [
        "build/ui_apps.bin",
        "main/build-debug/app.elf",
        "managed_components/lib/lib.c",
        ".venv/tool.py",
        "sdkconfig",
        "main/sdkconfig.local.defaults",
        ".env.local",
        "private.pem",
        "__pycache__/module.pyc",
    ]
    for name in bad:
        result = run_check(project, "artifacts", name)
        assert result.returncode == 1, name
        assert name in result.stderr
    good = [
        "ui/font.ttf",
        "assets/audio.wav",
        "assets/data.bin",
        "sdkconfig.application.defaults",
        "main/idf_component.yml",
        "dependencies.lock",
        ".env.example",
    ]
    assert run_check(project, "artifacts", *good).returncode == 0


def test_components_and_opt_in_forbidden_apis(project):
    component = project / "components/my-component"
    component.mkdir(parents=True)
    (component / "CMakeLists.txt").write_text('idf_component_register(SRCS "code.c")\n')
    (component / "code.c").write_text("void f(void) { gets(0); }\n")
    run_git(project, "add", ".")
    assert run_check(project, "components").returncode == 0
    policy = project / "tools/quality/policy.json"
    policy.write_text(json.dumps({"forbidden_apis": ["gets"]}))
    result = run_check(project, "components")
    assert result.returncode == 1 and "forbidden API: gets" in result.stderr
    (component / "code.c").write_text('// gets(0);\nconst char *s = "gets(0)";\n')
    assert run_check(project, "components").returncode == 0
    run_git(project, "rm", "--cached", "components/my-component/CMakeLists.txt")
    assert "stage its CMakeLists" in run_check(project, "components").stderr


def test_application_prefix_keeps_third_party_names_and_custom_templates_compatible(project):
    component = project / "components/my-component"
    vendor = project / "third_party/components/upstream-name"
    for directory in (component, vendor):
        directory.mkdir(parents=True)
        (directory / "CMakeLists.txt").write_text("idf_component_register()\n")
    run_git(project, "add", ".")
    # Existing custom templates need not adopt the built-in application profile.
    assert run_check(project, "components").returncode == 0
    policy = project / "tools/quality/policy.json"
    policy.write_text(json.dumps({"application_component_prefix": "app_"}))
    failure = run_check(project, "components")
    assert failure.returncode == 1 and "must start with app_" in failure.stderr
    assert "upstream-name" not in failure.stderr
    run_git(project, "mv", "components/my-component", "components/app_my-component")
    assert run_check(project, "components").returncode == 0
    policy.write_text(json.dumps({"application_component_prefix": ["app_"]}))
    assert "must be an ASCII identifier" in run_check(project, "components").stderr


def test_generated_checks_are_explicit_and_propagate_failure(project):
    assert "No generated-source checks" in run_check(project, "generated").stdout
    policy = project / "tools/quality/policy.json"
    for status in (0, 3):
        policy.write_text(json.dumps({"generated_checks": [["{python}", "-c", f"import sys; sys.exit({status})"]]}))
        assert run_check(project, "generated").returncode == (0 if status == 0 else 1)


def test_cppcheck_missing_tool_and_database_are_errors(project):
    result = run_check(project, "cppcheck", env={**os.environ, "PATH": ""})
    assert result.returncode == 1 and "Cppcheck is missing" in result.stderr


def test_shared_checker_uses_explicit_project_and_ignores_parent_git_environment(project, tmp_path):
    parent = tmp_path / "unrelated"
    parent.mkdir()
    run_git(parent, "init")
    (parent / "sdkconfig").write_text("unrelated parent content\n")
    run_git(parent, "add", ".")
    run_git(project, "add", ".")
    environment = {**os.environ, "GIT_DIR": str(parent / ".git"), "GIT_WORK_TREE": str(parent)}
    command = [sys.executable, str(TOOLS / "tools/quality/check_project.py"), "artifacts", "--project", str(project)]
    result = subprocess.run(command, cwd=parent, env=environment, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    (project / "sdkconfig").write_text("local output\n")
    run_git(project, "add", "-f", "sdkconfig")
    result = subprocess.run(command, cwd=parent, env=environment, capture_output=True, text=True)
    assert result.returncode == 1 and "sdkconfig" in result.stderr
