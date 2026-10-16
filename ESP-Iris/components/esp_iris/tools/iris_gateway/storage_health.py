from __future__ import annotations

import functools
import sqlite3
from typing import Any


class StorageUnavailable(RuntimeError):
    """Critical state could not be committed; no new device write is safe."""


def durable_write(method: Any) -> Any:
    @functools.wraps(method)
    def guarded(self: Any, *args: Any, **kwargs: Any) -> Any:
        try:
            result = method(self, *args, **kwargs)
        except (sqlite3.Error, OSError) as exc:
            self.storage_failed(exc)
            raise StorageUnavailable("gateway state could not be committed") from exc
        self._state_error = None
        return result
    return guarded
