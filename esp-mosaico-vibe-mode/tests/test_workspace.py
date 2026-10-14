from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest


TOOL_ROOT = Path(__file__).resolve().parents[2] / "mosaico-tools"
sys.path.insert(0, str(TOOL_ROOT / "tools"))

from mosaico_cli.errors import EnvironmentError
from mosaico_cli.workspace import CONFIG_NAME, load_workspace


def configuration() -> dict[str, object]:
    return {
        "schema_version": 2,
        "workspace": {
            "projects_dir": "apps",
            "default_project": "apps/demo",
            "run_dir": ".runs",
        },
        "dependencies": {
            "bsp": "third_party/bsp",
            "esp_iris": "third_party/esp-iris",
        },
        "build": {"runner": "builtin"},
        "devices": [
            {
                "id": "board",
                "name": "Board",
                "target": "esp32s31",
                "status": "supported",
                "default": True,
                "preview_target": True,
                "recovery_usb_ids": [],
            }
        ],
    }


class WorkspaceTests(unittest.TestCase):
    def test_recovery_build_resolves_monorepo_iris_checkout(self) -> None:
        manifest = TOOL_ROOT.parent / "esp-mosaico-vibe-mode" / "firmware" / "vibe-mode" / "main" / "idf_component.yml"
        manifest_text = manifest.read_text(encoding="utf-8")
        override = re.search(r"^\s*override_path:\s*(\S+)\s*$", manifest_text, re.MULTILINE)
        self.assertIsNotNone(override)
        component = (manifest.parent / override.group(1)).resolve()
        self.assertEqual(
            component,
            (TOOL_ROOT.parent / "ESP-Iris" / "components" / "esp_iris").resolve(),
        )
        self.assertTrue((component / "idf_component.yml").is_file())

        cmake = TOOL_ROOT.parent / "esp-mosaico-vibe-mode" / "firmware" / "vibe-mode" / "cmake" / "recovery_image.cmake"
        cmake_text = cmake.read_text(encoding="utf-8")
        bundle_tool = re.search(
            r'\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\n]+system_update_bundle\.py)',
            cmake_text,
        )
        self.assertIsNotNone(bundle_tool)
        self.assertEqual(
            (cmake.parent / bundle_tool.group(1)).resolve(),
            (
                TOOL_ROOT.parent
                / "ESP-Iris"
                / "components"
                / "esp_iris"
                / "tools"
                / "system_update_bundle.py"
            ).resolve(),
        )

    def test_discovers_workspace_above_current_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / "apps" / "demo" / "main"
            nested.mkdir(parents=True)
            (root / CONFIG_NAME).write_text(
                json.dumps(configuration()), encoding="utf-8"
            )

            workspace = load_workspace(TOOL_ROOT, start=nested)

            self.assertEqual(workspace.root, root.resolve())
            self.assertEqual(workspace.projects_dir, (root / "apps").resolve())
            self.assertIsNone(workspace.init_template)
            self.assertEqual(
                workspace.esp_iris_path, (root / "third_party" / "esp-iris").resolve()
            )
            self.assertTrue(str(workspace.build_runner).startswith(str(TOOL_ROOT)))
            self.assertEqual(
                workspace.recovery_project,
                TOOL_ROOT.parent / "esp-mosaico-vibe-mode" / "firmware" / "vibe-mode",
            )
            self.assertEqual(
                workspace.recovery_dir,
                TOOL_ROOT.parent / "esp-mosaico-vibe-mode" / "firmware" / "vibe-mode" / "prebuilt" / "recovery",
            )

    def test_explicit_workspace_accepts_config_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "custom.json"
            path.write_text(json.dumps(configuration()), encoding="utf-8")

            workspace = load_workspace(
                TOOL_ROOT, start=Path("/"), explicit=str(path)
            )

            self.assertEqual(workspace.config_path, path.resolve())
            self.assertEqual(workspace.root, root.resolve())

    def test_rejects_old_schema_without_modifying_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            value = configuration()
            value["schema_version"] = 1
            path = root / CONFIG_NAME
            original = json.dumps(value)
            path.write_text(original, encoding="utf-8")
            with self.assertRaisesRegex(EnvironmentError, "schema_version 2"):
                load_workspace(TOOL_ROOT, explicit=str(root))
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_rejects_unknown_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            value = configuration()
            value["schema_version"] = 99
            (root / CONFIG_NAME).write_text(json.dumps(value), encoding="utf-8")

            with self.assertRaises(EnvironmentError):
                load_workspace(TOOL_ROOT, explicit=str(root))

    def test_optional_init_template_is_resolved_from_workspace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            value = configuration()
            value["workspace"]["init_template"] = "templates/reference.json"
            (root / CONFIG_NAME).write_text(json.dumps(value), encoding="utf-8")
            workspace = load_workspace(TOOL_ROOT, explicit=str(root))
            self.assertEqual(workspace.init_template, (root / "templates/reference.json").resolve())

    def test_invalid_init_template_configuration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for template in (None, "", 42):
                value = configuration()
                value["workspace"]["init_template"] = template
                (root / CONFIG_NAME).write_text(json.dumps(value), encoding="utf-8")
                with self.subTest(template=template), self.assertRaises(EnvironmentError):
                    load_workspace(TOOL_ROOT, explicit=str(root))


if __name__ == "__main__":
    unittest.main()
