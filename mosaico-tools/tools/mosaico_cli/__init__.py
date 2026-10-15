"""Public implementation package for the ESP-Mosaico product CLI."""

from pathlib import Path
import sys

# Workspace launchers import this source package without installing the checkout.
_utils_root = str(Path(__file__).resolve().parents[3])
if _utils_root not in sys.path:
    sys.path.insert(0, _utils_root)

__version__ = "0.2.0"
