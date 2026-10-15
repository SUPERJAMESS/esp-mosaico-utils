# SPDX-License-Identifier: Apache-2.0
"""Load the public Iris extension even before this project's first build."""
import sys
from pathlib import Path

_tools = str(Path(__file__).resolve().parents[3] / "ESP-Iris/components/esp_iris/tools")
if _tools not in sys.path:
    sys.path.insert(0, _tools)

from iris_usb_flash import action_extensions  # noqa: F401
