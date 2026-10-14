from __future__ import annotations

from contextlib import ExitStack
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = REPO_ROOT / "tools" / "gsp-sim" / "run.py"
SPEC = importlib.util.spec_from_file_location("gsp_sim_run", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class GspSimRunTests(unittest.TestCase):
    def run_main(self, *arguments: str) -> tuple[int, list[str]]:
        with ExitStack() as contexts:
            contexts.enter_context(mock.patch.dict(MODULE.os.environ, {}, clear=True))
            contexts.enter_context(
                mock.patch.object(sys, "argv", [str(MODULE_PATH), "--project", str(REPO_ROOT / "templates/hello_world"), *arguments])
            )
            resolve_gsp_root = contexts.enter_context(
                mock.patch.object(MODULE, "resolve_gsp_root", return_value=None)
            )
            contexts.enter_context(
                mock.patch.object(MODULE, "resolve_gspc", return_value=Path("/tmp/gspc"))
            )
            contexts.enter_context(
                mock.patch.object(MODULE, "resolve_sim", return_value=Path("/tmp/sim"))
            )
            contexts.enter_context(mock.patch.object(MODULE, "pack_scene"))
            call = contexts.enter_context(
                mock.patch.object(MODULE.subprocess, "call", return_value=0)
            )
            result = MODULE.main()

        resolve_gsp_root.assert_called_once_with(REPO_ROOT / "templates/hello_world")
        command = call.call_args.args[0]
        return result, command

    def test_frames_and_fps_select_scene_only_without_component(self) -> None:
        result, command = self.run_main(
            "--headless", "--frames", "12", "--fps", "24"
        )

        self.assertEqual(result, 0)
        self.assertIn("--headless", command)
        self.assertEqual(command[command.index("--frames") + 1], "12")
        self.assertEqual(command[command.index("--fps") + 1], "24")

    def test_explicit_fps_selects_scene_only(self) -> None:
        result, command = self.run_main("--interactive", "--fps", "60")

        self.assertEqual(result, 0)
        self.assertEqual(command[command.index("--frames") + 1], "0")
        self.assertEqual(command[command.index("--fps") + 1], "60")

    def test_default_preview_requires_component_for_sim_bridge(self) -> None:
        with ExitStack() as contexts:
            contexts.enter_context(
                mock.patch.object(sys, "argv", [str(MODULE_PATH), "--project", str(REPO_ROOT / "templates/hello_world"), "--headless"])
            )
            contexts.enter_context(
                mock.patch.object(MODULE, "resolve_gsp_root", return_value=None)
            )
            with self.assertRaisesRegex(SystemExit, "esp-gsp is not installed"):
                MODULE.main()

    def test_default_preview_still_uses_sim_bridge(self) -> None:
        component = Path("/tmp/esp-gsp")
        with ExitStack() as contexts:
            contexts.enter_context(
                mock.patch.object(sys, "argv", [str(MODULE_PATH), "--project", str(REPO_ROOT / "templates/hello_world"), "--headless"])
            )
            contexts.enter_context(
                mock.patch.object(MODULE, "resolve_gsp_root", return_value=component)
            )
            contexts.enter_context(
                mock.patch.object(
                    MODULE, "sim_bridge_script", return_value=MODULE_PATH
                )
            )
            run_bridge = contexts.enter_context(
                mock.patch.object(MODULE, "run_sim_bridge", return_value=7)
            )
            result = MODULE.main()

        self.assertEqual(result, 7)
        run_bridge.assert_called_once_with(
            REPO_ROOT / "templates/hello_world/components/app_hello/host",
            component,
            project=REPO_ROOT / "templates/hello_world",
            headless=True,
        )


@pytest.mark.parametrize("layout", ["ui/main.json", "components/app_ui/ui/main.json"])
def test_scene_discovery_supports_legacy_and_component_layouts(tmp_path, layout):
    scene = tmp_path / layout
    scene.parent.mkdir(parents=True)
    scene.write_text("{}\n")
    (tmp_path / "sdkconfig.defaults").touch()
    assert MODULE.project_scene(tmp_path) == scene
    assert MODULE.scene_project(scene) == tmp_path


def test_scene_discovery_rejects_missing_and_ambiguous_entries(tmp_path):
    with pytest.raises(SystemExit, match="No scene found"):
        MODULE.project_scene(tmp_path)
    for name in ("app_first", "app_second"):
        scene = tmp_path / "components" / name / "ui/main.json"
        scene.parent.mkdir(parents=True)
        scene.write_text("{}\n")
    with pytest.raises(SystemExit, match="Multiple scenes.*app_first.*app_second"):
        MODULE.project_scene(tmp_path)
    (tmp_path / "ui").mkdir()
    (tmp_path / "ui/main.json").write_text("{}\n")
    with pytest.raises(SystemExit, match="Multiple scenes.*ui/main.json"):
        MODULE.project_scene(tmp_path)


def test_backend_belongs_to_selected_component_with_legacy_fallback(tmp_path):
    scene = tmp_path / "components/app_ui/ui/main.json"
    other = tmp_path / "components/app_other/host"
    other.mkdir(parents=True)
    (other / "CMakeLists.txt").touch()
    assert MODULE.project_backend(tmp_path, scene) is None
    legacy = tmp_path / "pc"
    legacy.mkdir()
    (legacy / "CMakeLists.txt").touch()
    assert MODULE.project_backend(tmp_path, scene) == legacy
    host = scene.parent.parent / "host"
    host.mkdir(parents=True)
    (host / "CMakeLists.txt").touch()
    assert MODULE.project_backend(tmp_path, scene) == host


@pytest.mark.parametrize("layout", ["pc", "components/app_ui/host"])
def test_explicit_backend_uses_application_dependencies_and_build_directory(tmp_path, layout):
    backend = tmp_path / layout
    backend.mkdir(parents=True)
    (backend / "CMakeLists.txt").touch()
    component = tmp_path / "managed_components/espressif__esp-gsp"
    arguments = [str(MODULE_PATH), "--pc-project", str(backend), "--headless", "--duration", "2"]
    with ExitStack() as contexts:
        contexts.enter_context(mock.patch.object(sys, "argv", arguments))
        resolve = contexts.enter_context(mock.patch.object(MODULE, "resolve_gsp_root", return_value=component))
        contexts.enter_context(mock.patch.object(MODULE, "sim_bridge_script", return_value=MODULE_PATH))
        contexts.enter_context(mock.patch.object(MODULE, "resolve_gspc", return_value=Path("gspc")))
        contexts.enter_context(mock.patch.object(MODULE, "resolve_sim", return_value=Path("sim")))
        call = contexts.enter_context(mock.patch.object(MODULE.subprocess, "call", return_value=0))
        assert MODULE.main() == 0
    resolve.assert_called_once_with(tmp_path)
    command = call.call_args.args[0]
    assert command[command.index("--project") + 1] == str(backend)
    assert command[command.index("--build-dir") + 1] == str(tmp_path / "build-sim")
    assert command[command.index("--duration") + 1] == "2.0"


@pytest.mark.parametrize("with_project", [False, True])
def test_explicit_scene_uses_owning_project_dependencies_not_pc_default(tmp_path, with_project):
    project = tmp_path / "application"
    scene = project / "components/app_other/ui/main.json"
    scene.parent.mkdir(parents=True)
    scene.write_text("{}\n")
    (project / "pc").mkdir()
    (project / "pc/CMakeLists.txt").touch()
    arguments = [str(MODULE_PATH), "--headless"]
    if with_project:
        arguments.extend(["--project", str(project), scene.relative_to(project).as_posix()])
    else:
        arguments.append(str(scene))
    with ExitStack() as contexts:
        contexts.enter_context(mock.patch.dict(MODULE.os.environ, {}, clear=True))
        contexts.enter_context(mock.patch.object(sys, "argv", arguments))
        resolve = contexts.enter_context(mock.patch.object(MODULE, "resolve_gsp_root", return_value=None))
        contexts.enter_context(mock.patch.object(MODULE, "resolve_gspc", return_value=Path("gspc")))
        contexts.enter_context(mock.patch.object(MODULE, "resolve_sim", return_value=Path("sim")))
        pack = contexts.enter_context(mock.patch.object(MODULE, "pack_scene"))
        bridge = contexts.enter_context(mock.patch.object(MODULE, "run_sim_bridge"))
        contexts.enter_context(mock.patch.object(MODULE.subprocess, "call", return_value=0))
        assert MODULE.main() == 0
    resolve.assert_called_once_with(project)
    assert pack.call_args.args[0] == scene
    bridge.assert_not_called()


if __name__ == "__main__":
    unittest.main()
