from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import replace
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


TOOL_ROOT = Path(__file__).resolve().parents[2] / "mosaico-tools"
sys.path.insert(0, str(TOOL_ROOT / "tools"))

from mosaico_cli.cli import main
from mosaico_cli.errors import EnvironmentError, OperationError, SelectionError
from mosaico_cli.scaffold import initialize_project
from mosaico_cli.project_defaults import management_files
from mosaico_cli.workspace import load_workspace


class ScaffoldTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.template = self.root / "templates" / "basic"
        self.template.mkdir(parents=True)
        config = {
            "schema_version": 2,
            "workspace": {
                "projects_dir": "apps/nested", "default_project": "apps/existing",
                "init_template": "templates/basic/description.json",
            },
            "dependencies": {"bsp": "vendor/board", "esp_iris": "vendor/transport"},
            "build": {"runner": "builtin"}, "devices": [{"id": "board"}],
        }
        (self.root / ".mosaico.json").write_text(json.dumps(config), encoding="utf-8")
        self.workspace = load_workspace(TOOL_ROOT, explicit=str(self.root))
        # No board-specific application, source language or shared script is
        # needed by this fixture: the descriptor owns the application layout.
        (self.template / "entry.txt").write_text("App: original\nLink: original\n", encoding="utf-8")
        (self.template / "payload.dat").write_bytes(b"\x00\xff\r\n")
        self.description = {
            "schema_version": 1,
            "paths": {"shared": {"base": "workspace", "path": "library with spaces"}},
            "files": [
                {"source": "entry.txt", "destination": "source/deep/start.txt", "replacements": [
                    {"pattern": "^App: original$", "replacement": "App: {{project_name}}"},
                    {"pattern": "^Link: original$", "replacement": "Link: {{shared|json}}"},
                ], "append": "Project: {{project_path}}\n"},
                {"source": "payload.dat", "destination": "assets/payload.bin"},
            ],
        }
        self.save_description()

    def save_description(self) -> None:
        self.workspace.init_template.write_text(json.dumps(self.description), encoding="utf-8")

    def snapshot(self) -> dict[str, bytes | None]:
        return {str(path.relative_to(self.root)): path.read_bytes() if path.is_file() else None
                for path in self.root.rglob("*")}

    def test_generates_declared_and_management_files_and_preserves_workspace(self) -> None:
        (self.template / "ignored.txt").write_text("local data", encoding="utf-8")
        before = self.snapshot()
        result = initialize_project(self.workspace, "My_app2")
        project = Path(result["project"])
        self.assertEqual(result["status"], "created")
        self.assertEqual(result["template"], str(self.workspace.init_template))
        self.assertEqual(project, self.root / "apps/nested/My_app2")
        self.assertEqual(result["files"], ["source/deep/start.txt", "assets/payload.bin", *management_files(self.workspace, Path(result["project"]))])
        self.assertEqual((project / "assets/payload.bin").read_bytes(), b"\x00\xff\r\n")
        text = (project / "source/deep/start.txt").read_text()
        self.assertIn("App: My_app2", text)
        self.assertIn("Project: apps/nested/My_app2", text)
        self.assertFalse((project / "main").exists())
        self.assertFalse((project / "ignored.txt").exists())
        after = self.snapshot()
        self.assertTrue(all(after[path] == content for path, content in before.items()))
        self.assertFalse(self.workspace.run_dir.exists())

    def test_paths_are_relative_to_each_destination_parent(self) -> None:
        workspace = replace(self.workspace, projects_dir=self.root / "apps with spaces/nested")
        self.description["files"].append({"source": "entry.txt", "destination": "entry.txt", "replacements": [
            {"pattern": "^Link: original$", "replacement": "Link: {{shared|json}}"},
        ]})
        self.save_description()
        project = Path(initialize_project(workspace, "demo")["project"])
        for filename in ("source/deep/start.txt", "entry.txt"):
            path = project / filename
            line = next(line for line in path.read_text().splitlines() if line.startswith("Link: "))
            reference = json.loads(line[len("Link: "):])
            self.assertEqual((path.parent / reference).resolve(), self.root / "library with spaces")

    def test_filters_preserve_cmake_and_shell_literals(self) -> None:
        self.description["paths"]["shared"]["path"] = "library$;folder"
        self.description["files"][0]["append"] = "{{shared|cmake}}\n{{project_path|shell}}\n"
        self.save_description()
        workspace = replace(self.workspace, projects_dir=self.root / "apps with spaces")
        result = initialize_project(workspace, "demo")
        lines = (Path(result["project"]) / "source/deep/start.txt").read_text().splitlines()
        self.assertIn(r"library\$\;folder", lines[-2])
        if sys.platform != "win32":
            import shlex
            self.assertEqual(shlex.split(lines[-1]), ["apps with spaces/demo"])
        else:
            self.assertEqual(lines[-1], '"apps with spaces/demo"')

    def test_only_descriptor_text_is_interpolated(self) -> None:
        (self.template / "entry.txt").write_text("App: original\nLink: original\n{{keep_source_literal}}\n", encoding="utf-8")
        result = initialize_project(self.workspace, "demo")
        self.assertIn("{{keep_source_literal}}", (Path(result["project"]) / "source/deep/start.txt").read_text())

    def test_dry_run_does_not_write_or_start_runtime(self) -> None:
        before = self.snapshot()
        output = io.StringIO()
        with mock.patch("mosaico_cli.cli.RunContext", side_effect=AssertionError("runtime started")), redirect_stdout(output):
            code = main(["project", "init", "demo", "--dry-run", "--json", "--verbose", "--workspace", str(self.root)])
        self.assertEqual(code, 0)
        result = json.loads(output.getvalue())
        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "dry_run")
        self.assertEqual(result["files"], ["source/deep/start.txt", "assets/payload.bin", *management_files(self.workspace, Path(result["project"]))])
        self.assertEqual(before, self.snapshot())

    def test_unconfigured_init_is_an_explicit_error(self) -> None:
        with self.assertRaisesRegex(EnvironmentError, "not configured"):
            initialize_project(replace(self.workspace, init_template=None), "demo")

    def test_required_resources_are_declared_by_the_template(self) -> None:
        self.description["paths"]["shared"]["required"] = "file"
        self.save_description()
        with self.assertRaisesRegex(EnvironmentError, "Required template resource"):
            initialize_project(self.workspace, "demo")
        self.assertFalse(self.workspace.projects_dir.exists())
        (self.root / "library with spaces").write_text("shared", encoding="utf-8")
        initialize_project(self.workspace, "demo")

    def test_invalid_descriptions_fail_without_writing(self) -> None:
        mutations = [
            {"schema_version": 2}, {"schema_version": True}, {"files": []},
            {"files": [{"source": "entry.txt", "replacements": "bad"}]},
            {"files": [{"source": "entry.txt", "replacements": [{"pattern": "[", "replacement": "x"}]}]},
            {"files": [{"source": "entry.txt", "append": "{{unknown}}"}]},
            {"files": [{"source": "entry.txt", "append": "{{project_name|unknown}}"}]},
            {"paths": {"project_name": {"base": "workspace", "path": "x"}}},
            {"paths": {"shared": {"base": "unknown", "path": "x"}}},
            {"paths": {"shared": {"base": "workspace", "path": "x", "required": True}}},
            {"execute": "do-not-run"},
        ]
        for mutation in mutations:
            value = {**self.description, **mutation}
            self.workspace.init_template.write_text(json.dumps(value), encoding="utf-8")
            before = self.snapshot()
            with self.subTest(mutation=mutation), self.assertRaises(EnvironmentError):
                initialize_project(self.workspace, "demo")
            self.assertEqual(before, self.snapshot())

    def test_file_paths_cannot_escape_or_collide(self) -> None:
        for field in ("source", "destination"):
            for path in ("../outside", "/absolute", "C:/file", "x\\file", "x/../file", "CON", "a:stream", "a*b", "a?b"):
                self.description["files"] = [{"source": "entry.txt", field: path}]
                self.save_description()
                with self.subTest(field=field, path=path), self.assertRaises(EnvironmentError):
                    initialize_project(self.workspace, "demo")
        for destination in ("same", "Same", "same/nested"):
            self.description["files"] = [{"source": "entry.txt", "destination": "same"},
                                         {"source": "entry.txt", "destination": destination}]
            self.save_description()
            with self.subTest(destination=destination), self.assertRaises(EnvironmentError):
                initialize_project(self.workspace, "demo")
        self.assertFalse(self.workspace.projects_dir.exists())

    def test_rejects_bad_names_before_writing(self) -> None:
        before = self.snapshot()
        for name in ("", "../escape", "a/b", "a\\b", "1app", "app-name", "CON", "lpt1", "应用", "a" * 32):
            with self.subTest(name=name), self.assertRaises(SelectionError):
                initialize_project(self.workspace, name)
        self.assertEqual(before, self.snapshot())

    def test_rejects_existing_file_and_empty_directory(self) -> None:
        self.workspace.projects_dir.mkdir(parents=True)
        for name in ("file", "directory"):
            target = self.workspace.projects_dir / name
            if name == "file":
                target.write_text("keep", encoding="utf-8")
            else:
                target.mkdir()
            before = self.snapshot()
            with self.assertRaises(SelectionError):
                initialize_project(self.workspace, name)
            self.assertEqual(before, self.snapshot())

    def test_rejects_broken_symlink_destination(self) -> None:
        self.workspace.projects_dir.mkdir(parents=True)
        target = self.workspace.projects_dir / "demo"
        try:
            target.symlink_to(self.root / "missing", target_is_directory=True)
        except OSError:
            self.skipTest("Symlinks are unavailable")
        with self.assertRaises(SelectionError):
            initialize_project(self.workspace, "demo")
        self.assertTrue(target.is_symlink())

    def test_rejects_destination_outside_workspace_and_recovery_template(self) -> None:
        for workspace in (replace(self.workspace, projects_dir=self.root.parent / "outside"),
                          replace(self.workspace, init_template=self.workspace.recovery_project)):
            with self.assertRaises(SelectionError):
                initialize_project(workspace, "demo")

    def test_incomplete_template_fails_before_writing(self) -> None:
        (self.template / "payload.dat").unlink()
        before = self.snapshot()
        with self.assertRaises(EnvironmentError):
            initialize_project(self.workspace, "demo")
        self.assertEqual(before, self.snapshot())

    def test_dry_run_rejects_a_file_in_the_destination_parent_path(self) -> None:
        (self.root / "apps").write_text("keep", encoding="utf-8")
        before = self.snapshot()
        with self.assertRaises(EnvironmentError):
            initialize_project(self.workspace, "demo", dry_run=True)
        self.assertEqual(before, self.snapshot())

    def test_replacement_count_is_enforced(self) -> None:
        source = self.template / "entry.txt"
        source.write_text(source.read_text() + "App: original\n", encoding="utf-8")
        with self.assertRaisesRegex(EnvironmentError, "expected 1 match"):
            initialize_project(self.workspace, "demo")
        self.assertFalse(self.workspace.projects_dir.exists())
        self.description["files"][0]["replacements"][0]["count"] = 2
        self.save_description()
        result = initialize_project(self.workspace, "demo")
        self.assertEqual((Path(result["project"]) / "source/deep/start.txt").read_text().count("App: demo"), 2)

    def test_crlf_template_is_supported(self) -> None:
        for path in self.template.rglob("*"):
            if path.is_file():
                path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        result = initialize_project(self.workspace, "demo")
        self.assertIn("App: demo", (Path(result["project"]) / "source/deep/start.txt").read_text())

    def test_write_failure_cleans_created_files_and_parents(self) -> None:
        before = self.snapshot()
        original = Path.open

        def fail(path, mode="r", *args, **kwargs):
            if mode == "xb" and path.name == "payload.bin":
                raise OSError("simulated disk full")
            return original(path, mode, *args, **kwargs)

        with mock.patch.object(Path, "open", fail), self.assertRaises(OperationError) as caught:
            initialize_project(self.workspace, "demo")
        self.assertEqual(caught.exception.details["cleanup_errors"], [])
        self.assertEqual(before, self.snapshot())

    def test_concurrent_initialization_has_one_winner(self) -> None:
        def create():
            try:
                return initialize_project(self.workspace, "demo")["status"]
            except SelectionError:
                return "conflict"

        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: create(), range(2)))
        self.assertCountEqual(results, ["created", "conflict"])
        self.assertEqual(len(list((self.workspace.projects_dir / "demo").rglob("*.bin"))), 1)

    def test_failure_cleanup_preserves_content_created_by_someone_else(self) -> None:
        project = self.workspace.projects_dir / "demo"
        foreign = project / "keep.txt"
        original = Path.open

        def fail(path, mode="r", *args, **kwargs):
            if mode == "xb" and path.name == "payload.bin":
                foreign.write_text("external content", encoding="utf-8")
                raise OSError("simulated disk full")
            return original(path, mode, *args, **kwargs)

        with mock.patch.object(Path, "open", fail), self.assertRaises(OperationError) as caught:
            initialize_project(self.workspace, "demo")
        self.assertEqual(foreign.read_text(), "external content")
        self.assertEqual(list(project.iterdir()), [foreign])
        self.assertTrue(caught.exception.details["cleanup_errors"])

    def test_cli_json_error_and_success(self) -> None:
        for name, expected in (("demo", 0), ("demo", 2), ("../outside", 2)):
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                code = main(["--workspace", str(self.root), "project", "init", name, "--json"])
            self.assertEqual(code, expected)
            result = json.loads(out.getvalue() if code == 0 else err.getvalue())
            self.assertEqual(result["ok"], code == 0)
            if code:
                self.assertEqual(result["error"], "selection_error")
                self.assertEqual(result["exit_code"], 2)


if __name__ == "__main__":
    unittest.main()
