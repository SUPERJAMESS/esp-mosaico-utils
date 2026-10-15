"""Allow shared tests to run from any working directory without installation."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
