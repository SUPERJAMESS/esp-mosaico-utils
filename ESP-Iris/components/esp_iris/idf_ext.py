# SPDX-License-Identifier: Apache-2.0
"""Load Iris's native flash/monitor integration through IDF component discovery."""
import sys
from pathlib import Path

_tools = str(Path(__file__).resolve().parent / "tools")
if _tools not in sys.path:
    sys.path.insert(0, _tools)

from iris_usb_flash import action_extensions  # noqa: F401
