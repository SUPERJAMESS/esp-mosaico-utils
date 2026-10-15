# SPDX-License-Identifier: Apache-2.0
"""Load the public Iris extension even before this project's first build."""
import sys
from pathlib import Path

_tools = str(Path(__file__).resolve().parents[3] / "ESP-Iris/components/esp_iris/tools")
if _tools not in sys.path:
    sys.path.insert(0, _tools)

from iris_usb_flash import action_extensions as iris_action_extensions


def action_extensions(base_actions, project_path):
    iris_action_extensions(base_actions, project_path)
    # The POSIX host worker passes verified lock descriptors to IDF. Keep the
    # existing Windows target until native flashing supports its handle handoff.
    if sys.platform == "win32":
        return {}
    flash = base_actions.get("actions", {}).get("flash")
    if flash is None:
        return {}
    # Use IDF's flash callback with Iris's identity-checked USB handoff, while
    # retaining the reviewed/current bundle selected by this CMake target.
    return {"version": "1", "actions": {"mosaico-recover-flash": dict(
        flash, help="Install the selected Vibe Mode base bundle over USB.")}}
