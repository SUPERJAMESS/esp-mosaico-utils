"""Public display-resource API; optional rendering dependencies load on use."""
from __future__ import annotations

from pathlib import Path
from typing import Dict, Optional


def generate_resources(*, name: str, description: str, locale: str,
                       font: Path, icon: Optional[Path] = None) -> Dict[str, bytes]:
    """Return owned resource bytes; no writes or shared mutable rendering state.
    Inputs follow display/1 limits; invalid input raises ValueError, I/O raises OSError.
    """
    from .presentation import generate_resources as generate
    return generate(name=name, description=description, locale=locale, font=font, icon=icon)
