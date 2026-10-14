"""The single import boundary to the pinned, public Iris host API."""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

from .errors import EnvironmentError


def host_api(workspace: Any) -> Any:
    tools = workspace.esp_iris_path / "components/esp_iris/tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    from iris_gateway import client

    if Path(client.__file__).resolve().parent.parent != tools.resolve():
        raise EnvironmentError("Another Iris checkout is already imported in this host process")
    if client.API_MAJOR != 2:
        raise EnvironmentError("Unsupported Iris public host API version")
    return client


def load_bundle(source: Any) -> Any:
    """Read a local bundle through the pinned Iris bundle API, without a device."""
    from types import SimpleNamespace
    checkout = Path(__file__).resolve().parents[3] / "ESP-Iris"
    return host_api(SimpleNamespace(esp_iris_path=checkout)).inspect_system_update_bundle(source)
