"""Exercise generated repositories and shared quality checks without a workspace checkout."""

from dataclasses import replace
import json
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "tools"))

from mosaico_cli.cli import main
from mosaico_cli.errors import EnvironmentError, OperationError
from mosaico_cli.project_defaults import management_files
from mosaico_cli.project_git import git_executable
from mosaico_cli.scaffold import finish_engine_project, initialize_project
from mosaico_cli.workspace import load_workspace


@pytest.fixture
def workspace():
    # Same drive as utils for Windows relative template references.
    with TemporaryDirectory(prefix=".project-quality-test-", dir=TOOLS.parent) as temporary:
        root = Path(temporary).resolve()
        (root / "templates").mkdir()
        (root / "templates/entry.txt").write_bytes(b"Application\n")
        (root / "templates/template.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "files": [{"source": "entry.txt"}],
                }
            ),
            encoding="utf-8",
        )
        (root / ".mosaico.json").write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "workspace": {"projects_dir": "projects", "init_template": "templates/template.json"},
                    "dependencies": {"bsp": "board", "raylib": "engine", "esp_iris": "iris"},
                    "build": {"runner": "builtin"},
                    "devices": [{"id": "board"}],
                }
            ),
            encoding="utf-8",
        )
        engine = root / "engine"
        (engine / "tools").mkdir(parents=True)
        (engine / "idf_component.yml").touch()
        (engine / "CMakeLists.txt").touch()
        # Real process boundary, independent of a consuming workspace or Engine checkout.
        (engine / "tools/game_cli.py").write_text(
            "import json, pathlib, sys\n"
            "project = pathlib.Path(sys.argv[2])\n"
            "files = {'CMakeLists.txt': '# example\\n', '.gitignore': '# engine policy\\n'}\n"
            "if '--dry-run' not in sys.argv:\n"
            "    project.mkdir(parents=True)\n"
            "    for name, content in files.items():\n"
            "        (project / name).write_text(content)\n"
            "print(json.dumps({'project': str(project), 'files': list(files)}))\n",
            encoding="utf-8",
        )
        yield load_workspace(TOOLS, explicit=str(root))


def run_git(project, *arguments):
    return subprocess.run(["git", "-C", str(project), *arguments], capture_output=True, text=True, check=True)


def add_template_file(workspace, name, content):
    source = workspace.init_template.parent / name
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(content, encoding="utf-8")
    description = json.loads(workspace.init_template.read_text())
    description["files"].append({"source": name})
    workspace.init_template.write_text(json.dumps(description))


def test_independent_git_without_commit_index_remote_or_hooks(workspace, monkeypatch):
    run_git(workspace.root, "init")
    parent_config = (workspace.root / ".git/config").read_bytes()
    parent_head = (workspace.root / ".git/HEAD").read_bytes()
    monkeypatch.setenv("GIT_DIR", str(workspace.root / ".git"))
    monkeypatch.setenv("GIT_WORK_TREE", str(workspace.root))
    monkeypatch.setenv("GIT_INDEX_FILE", str(workspace.root / ".git/custom-index"))
    monkeypatch.setenv("GIT_CONFIG_COUNT", "1")
    monkeypatch.setenv("GIT_CONFIG_KEY_0", "init.defaultBranch")
    monkeypatch.setenv("GIT_CONFIG_VALUE_0", "unexpected")
    result = initialize_project(workspace, "demo")
    monkeypatch.undo()
    project = Path(result["project"])
    assert Path(run_git(project, "rev-parse", "--show-toplevel").stdout.strip()).resolve() == project
    assert run_git(project, "symbolic-ref", "--short", "HEAD").stdout.strip() == "main"
    assert run_git(project, "ls-files").stdout == ""
    assert run_git(project, "remote").stdout == ""
    assert not (project / ".git/refs/heads/main").exists()
    assert not (project / ".git/hooks/pre-commit").exists()
    assert not (workspace.root / ".git/custom-index").exists()
    assert (workspace.root / ".git/config").read_bytes() == parent_config
    assert (workspace.root / ".git/HEAD").read_bytes() == parent_head
    assert result["git"] == {"status": "initialized", "branch": "main"}
    assert all(not name.startswith(".git/") for name in result["files"])


def test_preserve_template_configuration(workspace):
    for name in (".gitignore", ".pre-commit-config.yaml", "tools/quality/policy.json"):
        add_template_file(workspace, name, "custom content\n")
    result = initialize_project(workspace, "demo")
    assert set(result["quality"]["preserved_files"]) == {
        ".gitignore",
        ".pre-commit-config.yaml",
        "tools/quality/policy.json",
    }
    for name in result["quality"]["preserved_files"]:
        assert (Path(result["project"]) / name).read_text() == "custom content\n"
    assert (Path(result["project"]) / ".gitattributes").is_file()


@pytest.mark.parametrize(
    "name",
    [
        ".git/config",
        ".GIT/HEAD",
        "nested/.git/config",
        "tools/quality",
        ".gitignore/child",
        ".GitIgnore",
        "Tools/custom.py",
    ],
)
def test_conflicts_and_git_metadata_fail_before_writing(workspace, name):
    add_template_file(workspace, name, "content\n")
    with pytest.raises(EnvironmentError):
        initialize_project(workspace, "demo")
    assert not workspace.projects_dir.exists()


def test_missing_git_preflight_and_init_failure_retains_sources(workspace, monkeypatch):
    monkeypatch.setattr("mosaico_cli.project_git.shutil.which", lambda name: None)
    with pytest.raises(EnvironmentError, match="requires Git"):
        initialize_project(workspace, "demo")
    assert not workspace.projects_dir.exists()
    monkeypatch.undo()
    monkeypatch.setattr(
        "mosaico_cli.project_git.subprocess.run", lambda *a, **kw: SimpleNamespace(returncode=1, stderr="disk full")
    )
    with pytest.raises(OperationError, match="retained") as failure:
        initialize_project(workspace, "demo")
    assert (workspace.projects_dir / "demo/entry.txt").is_file()
    assert failure.value.details["git_initialized"] is False
    assert "init --initial-branch=main" in failure.value.details["recovery_command"]


def test_dry_run_has_complete_files_and_no_git_process(workspace, monkeypatch):
    monkeypatch.setattr("mosaico_cli.project_git.subprocess.run", lambda *a, **kw: pytest.fail("Git invoked"))
    result = initialize_project(workspace, "demo", dry_run=True)
    assert set(result["files"]) == {"entry.txt", *management_files(workspace, workspace.projects_dir / "demo")}
    assert result["git"]["status"] == "planned"
    assert not workspace.projects_dir.exists()


def test_shared_checker_is_referenced_without_copying_implementation(workspace):
    result = initialize_project(workspace, "demo")
    project = Path(result["project"])
    assert not (project / "tools/quality/check_project.py").exists()
    entries = [
        shlex.split(line.strip())
        for line in (project / ".pre-commit-config.yaml").read_text().splitlines()
        if line.strip().startswith("python ")
    ]
    assert len(entries) == 4
    for entry in entries:
        assert (project / entry[1]).resolve() == TOOLS / "tools/quality/check_project.py"
        assert not Path(entry[1]).is_absolute()
    assert set(path.name for path in (project / "tools/quality").iterdir()) == {
        "README.md",
        "policy.json",
        "requirements.txt",
        "astyle.yml",
        "ruff.toml",
    }


@pytest.mark.parametrize("template", ["hello_world", "blank_game"])
def test_builtin_layout_and_ignored_lockfile(workspace, template):
    selected = replace(workspace, init_template=TOOLS / "templates" / template / "mosaico-template.json")
    planned = initialize_project(selected, "demo", dry_run=True)
    result = initialize_project(selected, "demo")
    assert planned["files"] == result["files"]
    project = Path(result["project"])
    for name in ("third_party", "ui", "pc", ".astyle-rules.yml", ".ruff.toml"):
        assert not (project / name).exists()
    (project / "dependencies.lock").write_text("generated\n")
    assert run_git(project, "check-ignore", "dependencies.lock").stdout.strip() == "dependencies.lock"
    run_git(project, "add", ".")
    assert "dependencies.lock" not in run_git(project, "ls-files").stdout.splitlines()
    if template == "hello_world":
        assert (project / "components/app_hello/ui/main.json").is_file()
        assert (project / "components/app_hello/ui/fonts/LICENSE").is_file()
        assert (project / "components/app_hello/host/CMakeLists.txt").is_file()


@pytest.mark.parametrize("ruff", [".ruff.toml", "ruff.toml", "pyproject.toml"])
def test_existing_root_format_rules_keep_native_discovery(workspace, ruff):
    add_template_file(workspace, ruff, "# template-owned\n")
    add_template_file(workspace, ".astyle-rules.yml", "# template-owned\n")
    planned = initialize_project(workspace, "demo", dry_run=True)
    result = initialize_project(workspace, "demo")
    assert planned["files"] == result["files"]
    assert result["quality"]["preserved_files"] == [".astyle-rules.yml", ruff]
    project = Path(result["project"])
    config = (project / ".pre-commit-config.yaml").read_text()
    assert "--rules=.astyle-rules.yml" in config
    assert "--config" not in config
    assert not (project / "tools/quality/ruff.toml").exists()
    assert not (project / "tools/quality/astyle.yml").exists()
    assert (project / ruff).read_text() == "# template-owned\n"


def test_template_quality_rules_take_precedence_without_rewriting_custom_hooks(workspace):
    for name in ("tools/quality/ruff.toml", "tools/quality/astyle.yml", ".ruff.toml", ".astyle-rules.yml"):
        add_template_file(workspace, name, "# template-owned\n")
    result = initialize_project(workspace, "demo")
    config = (Path(result["project"]) / ".pre-commit-config.yaml").read_text()
    assert "--config, tools/quality/ruff.toml" in config
    assert "--rules=tools/quality/astyle.yml" in config
    custom = "repos: []\n"
    add_template_file(workspace, ".pre-commit-config.yaml", custom)
    result = initialize_project(workspace, "custom")
    assert (Path(result["project"]) / ".pre-commit-config.yaml").read_text() == custom


def test_engine_preserves_root_rules_in_dry_run_and_written_hooks(workspace):
    project = workspace.projects_dir / "demo"
    common = management_files(workspace, project)
    sources = {".ruff.toml": b"# existing\n", ".astyle-rules.yml": b"# existing\n"}
    planned = finish_engine_project(workspace, project, {"files": list(sources)}, common,
                                    git_executable(), dry_run=True)
    project.mkdir(parents=True)
    for name, content in sources.items():
        (project / name).write_bytes(content)
    created = finish_engine_project(workspace, project, {}, common, git_executable(), dry_run=False)
    assert set(planned["files"]) == set(created["files"])
    assert planned["quality"]["preserved_files"] == created["quality"]["preserved_files"]
    config = (project / ".pre-commit-config.yaml").read_text()
    assert "--rules=.astyle-rules.yml" in config and "--config" not in config
    assert not (project / "tools/quality/ruff.toml").exists()


def test_project_sim_forwards_explicit_scene_after_options(workspace, monkeypatch):
    add_template_file(workspace, "CMakeLists.txt", "project(demo)\n")
    project = Path(initialize_project(workspace, "demo")["project"])
    calls = []
    monkeypatch.setattr("mosaico_cli.app_commands.subprocess.call", lambda command: calls.append(command) or 0)
    assert main(["--workspace", str(workspace.root), "project", "sim", "--project", str(project),
                 "--headless", "--frames", "2", "--scene", "components/app_ui/ui/main.json"]) == 0
    assert calls[0][2:] == ["--project", str(project), "--headless", "--frames", "2", "components/app_ui/ui/main.json"]


def test_missing_shared_checker_fails_before_writing(workspace, monkeypatch):
    is_file = Path.is_file
    checker = TOOLS / "tools/quality/check_project.py"
    monkeypatch.setattr(Path, "is_file", lambda path: False if path == checker else is_file(path))
    with pytest.raises(EnvironmentError, match="Required template resource"):
        initialize_project(workspace, "demo")
    assert not workspace.projects_dir.exists()


@pytest.mark.parametrize("action", ["create", "new"])
@pytest.mark.parametrize("template", ["shooter", "sky-hop", "tower-defense"])
def test_engine_setup_and_dry_run(workspace, capsys, action, template):
    arguments = ["--workspace", str(workspace.root), "game", action, "demo", "--template", template, "--json"]
    assert main([*arguments, "--dry-run"]) == 0
    planned = json.loads(capsys.readouterr().out)
    assert planned["git"]["status"] == "planned"
    assert planned["quality"]["preserved_files"] == [".gitignore"]
    assert not workspace.projects_dir.exists()
    assert main(arguments) == 0
    created = json.loads(capsys.readouterr().out)
    assert set(planned["files"]) == set(created["files"])
    project = Path(created["project"])
    assert (project / ".git").is_dir()
    assert (project / ".gitignore").read_text() == "# engine policy\n"
    assert main(arguments) != 0
    assert (project / ".gitignore").read_text() == "# engine policy\n"


@pytest.mark.parametrize("name", [".gitignore", ".GitIgnore", "Tools"])
def test_engine_empty_directory_conflicts_preserve_sources(workspace, name):
    project = workspace.projects_dir / "demo"
    (project / name).mkdir(parents=True)
    (project / "main.c").write_text("// engine source\n")
    with pytest.raises(EnvironmentError):
        finish_engine_project(
            workspace,
            project,
            {},
            management_files(workspace, workspace.projects_dir / "demo"),
            git_executable(),
            dry_run=False,
        )
    assert (project / "main.c").read_text() == "// engine source\n"
    assert not (project / ".git").exists()


def test_game_dry_run_text_shows_management_files(workspace, capsys):
    assert main(["--workspace", str(workspace.root), "game", "new", "demo", "--dry-run"]) == 0
    output = capsys.readouterr().out
    assert ".pre-commit-config.yaml" in output and "Git: planned" in output
    assert not workspace.projects_dir.exists()


@pytest.fixture
def project(workspace):
    return Path(initialize_project(workspace, "demo")["project"])


HOOKS_ENABLED = os.environ.get("MOSAICO_TEST_PRE_COMMIT") == "1"


def run_hook(project, hook=None, *arguments):
    command = [sys.executable, "-m", "pre_commit", "run"]
    if hook:
        command.append(hook)
    return subprocess.run([*command, "--all-files", *arguments], cwd=project, capture_output=True, text=True)


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
@pytest.mark.parametrize("config", [".ruff.toml", "ruff.toml", "pyproject.toml"])
def test_real_ruff_respects_template_root_and_nested_rules(workspace, config):
    prefix = "tool.ruff." if config == "pyproject.toml" else ""
    add_template_file(workspace, config, f'[{prefix}lint]\nignore = ["F401"]\n[{prefix}format]\nquote-style = "single"\n')
    add_template_file(workspace, "code.py", "import os\n\nvalue = 'custom formatting'\n")
    add_template_file(workspace, "nested/ruff.toml", '[lint]\nignore = ["F821"]\n')
    add_template_file(workspace, "nested/code.py", "undefined_name()\n")
    project = Path(initialize_project(workspace, "demo")["project"])
    run_git(project, "add", ".")
    for hook in ("ruff", "ruff-format"):
        result = run_hook(project, hook)
        assert result.returncode == 0, result.stdout + result.stderr
    assert run_git(project, "diff", "--name-only").stdout == ""


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
@pytest.mark.parametrize("template", ["hello_world", "blank_game"])
def test_builtin_templates_pass_real_hooks_twice(workspace, template):
    selected = replace(workspace, init_template=TOOLS / "templates" / template / "mosaico-template.json")
    project = Path(initialize_project(selected, "demo")["project"])
    run_git(project, "add", ".")
    for _ in range(2):
        result = run_hook(project)
        assert result.returncode == 0, result.stdout + result.stderr
        assert run_git(project, "diff", "--name-only").stdout == ""


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_real_shared_hooks_survive_workspace_relocation_and_quoted_paths(workspace):
    # Model a dependency checkout and project being cloned in another workspace.
    source = workspace.root / "utils with spaces and 'quote'"
    tools = source / "mosaico-tools"
    shutil.copytree(TOOLS / "templates/common", tools / "templates/common")
    shutil.copytree(TOOLS / "tools/quality", tools / "tools/quality")
    selected = replace(workspace, tool_root=tools)
    project = Path(initialize_project(selected, "demo")["project"])
    moved = workspace.root / "relocated workspace"
    (moved / "projects").mkdir(parents=True)
    source.rename(moved / source.name)
    project.rename(moved / "projects/demo")
    project = moved / "projects/demo"
    run_git(project, "add", ".")
    assert run_hook(project, "mosaico-artifacts").returncode == 0
    assert run_hook(project, "mosaico-components").returncode == 0
    (project / "build").mkdir()
    (project / "build/forced.txt").write_text("output\n")
    run_git(project, "add", "-f", "build/forced.txt")
    result = run_hook(project, "mosaico-artifacts")
    assert result.returncode == 1 and "do not commit build directory" in result.stdout


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
@pytest.mark.parametrize(
    "hook,name,content",
    [
        ("trailing-whitespace", "bad.txt", "bad   \n"),
        ("end-of-file-fixer", "bad.txt", "missing newline"),
        ("mixed-line-ending", "bad.txt", "one\r\ntwo\n"),
        ("check-yaml", "bad.yaml", "value: [\n"),
        ("check-json", "bad.json", "{bad}\n"),
        ("check-toml", "bad.toml", "[broken\n"),
        ("ruff", "bad.py", "import unused_module\n"),
        ("astyle_py", "bad.c", "int f(void){return 0;}\n"),
        ("detect-private-key", "key.txt", "-----BEGIN RSA PRIVATE KEY-----\nexample\n"),
        ("detect-secrets", "credentials.py", 'password = "not-a-real-password-for-testing"\n'),
        ("check-merge-conflict", "bad.txt", "<<<<<<< HEAD\nleft\n=======\nright\n>>>>>>> branch\n"),
        ("check-added-large-files", "large.txt", "x" * (5120 * 1024 + 1)),
        ("mosaico-artifacts", "build/forced.txt", "build output\n"),
    ],
    ids=[
        "whitespace",
        "eof",
        "lf",
        "yaml",
        "json",
        "toml",
        "ruff",
        "astyle",
        "private-key",
        "secrets",
        "conflicts",
        "large-file",
        "forced-artifact",
    ],
)
def test_real_hooks_reject_defects(project, hook, name, content):
    path = project / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content.encode())
    run_git(project, "add", "-f", name)
    result = run_hook(project, hook)
    assert result.returncode == 1, result.stdout + result.stderr


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_cppcheck_real_analysis_filters_external_sources(project):
    source = project / "main.c"
    source.write_text("int main(void) { return 0; }\n")
    database = project / "custom-db.json"
    database.write_text(
        json.dumps(
            [
                {"directory": str(project), "file": str(source), "arguments": ["cc", "-c", str(source)]},
                {"directory": str(project.parent), "file": "external.c", "arguments": ["cc", "-c", "external.c"]},
            ]
        )
    )
    (project / "tools/quality/policy.json").write_text(json.dumps({"compile_commands": "custom-db.json"}))
    result = run_hook(project, "mosaico-cppcheck", "--hook-stage", "manual")
    assert result.returncode == 0, result.stdout + result.stderr
    source.write_text("int main(void) { int *p = 0; return *p; }\n")
    result = run_hook(project, "mosaico-cppcheck", "--hook-stage", "manual")
    assert result.returncode == 1 and "nullPointer" in result.stdout, result.stdout + result.stderr
    database.unlink()
    result = run_hook(project, "mosaico-cppcheck", "--hook-stage", "manual")
    assert result.returncode == 1 and "database missing" in result.stdout


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_real_file_conventions(project):
    script = project / "script.py"
    script.write_text("#!/usr/bin/env python3\nprint('ok')\n")
    run_git(project, "add", "--chmod=-x", "script.py")
    result = run_hook(project, "check-shebang-scripts-are-executable")
    assert result.returncode == 1 and "not marked executable" in result.stdout
    script.chmod(0o755)
    run_git(project, "add", "--chmod=+x", "script.py")
    assert run_hook(project, "check-shebang-scripts-are-executable").returncode == 0
    script.write_text("print('ok')\n")
    run_git(project, "add", "--chmod=+x", "script.py")
    result = run_hook(project, "check-executables-have-shebangs")
    assert result.returncode == 1 and "no (or invalid) shebang" in result.stdout


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_real_case_conflicts(project):
    (project / "case.txt").write_text("case\n")
    run_git(project, "add", "case.txt")
    blob = run_git(project, "hash-object", "case.txt").stdout.strip()
    # Stage both spellings even on case-insensitive Windows/macOS filesystems.
    (project / "Case.txt").write_text("case\n")
    run_git(project, "update-index", "--add", "--cacheinfo", f"100644,{blob},Case.txt")
    result = run_hook(project, "check-case-conflict")
    assert result.returncode == 1, result.stdout + result.stderr


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_real_broken_symlinks(project):
    link = project / "link.txt"
    try:
        link.symlink_to("missing.txt")
    except OSError:
        pytest.skip("Host cannot create symlinks")
    run_git(project, "add", "link.txt")
    assert run_hook(project, "check-symlinks").returncode == 1


@pytest.mark.skipif(not HOOKS_ENABLED, reason="Set MOSAICO_TEST_PRE_COMMIT=1 for real pinned hook environments")
def test_real_markdown_linebreaks_and_binary_assets(project):
    (project / "notes.md").write_bytes(b"one  \ntwo\n")
    (project / "asset.bin").write_bytes(b"\x00\xff\x01\r\n")
    run_git(project, "add", ".")
    result = run_hook(project)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (project / "notes.md").read_bytes() == b"one  \ntwo\n"
    assert (project / "asset.bin").read_bytes() == b"\x00\xff\x01\r\n"
