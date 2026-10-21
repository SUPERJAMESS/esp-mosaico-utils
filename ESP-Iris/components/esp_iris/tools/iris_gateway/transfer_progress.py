"""Bound durable byte-counter updates without delaying phase evidence."""
from __future__ import annotations

import time
from typing import Any


class TransferProgress:
    """Report byte counters at most once a second for each active transfer.

    Phase/component changes and the final byte count bypass the interval.
    Admission, commit receipts and terminal operation results remain durable
    independently of these sampled counters.
    """

    def __init__(self) -> None:
        self._key: tuple[str, Any] | None = None
        self._updated = 0.0

    def accept(self, progress: dict[str, Any]) -> bool:
        key = (str(progress.get("stage", "transferring")), progress.get("component_id"))
        now = time.monotonic()
        total = int(progress.get("bytes_total", 0))
        complete = total > 0 and int(progress.get("bytes_received", 0)) >= total
        if key == self._key and not complete and now - self._updated < 1.0:
            return False
        self._key, self._updated = key, now
        return True
