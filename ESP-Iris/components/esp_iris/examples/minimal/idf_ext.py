# SPDX-License-Identifier: Apache-2.0
"""Explicitly load the local Iris extension, including before the first build."""
import sys
from pathlib import Path

_tools = str(Path(__file__).resolve().parents[2] / "tools")
if _tools not in sys.path:
    sys.path.insert(0, _tools)

from iris_usb_flash import action_extensions  # noqa: F401
