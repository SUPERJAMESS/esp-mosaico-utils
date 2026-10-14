"""Keep generated IDF dependencies out of the Recovery host test suite."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mosaico-tools/tools"))

collect_ignore_glob = [
    "firmware/*/managed_components",
    "firmware/*/build*",
]
