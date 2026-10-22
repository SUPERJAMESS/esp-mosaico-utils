"""Bound durable byte-counter updates without delaying phase evidence."""
from __future__ import annotations

import time
from typing import Any


class TransferProgress:
    """Report byte counters at most once a second for each active transfer.

    Keep one-percent sampling; phase/component changes and the final byte
    count bypass both bounds.
    Admission, commit receipts and terminal operation results remain durable
    independently of these sampled counters.
    """

    def __init__(self) -> None:
        self._key: tuple[str, Any] | None = None
        self._updated = 0.0
        self._progress = -10

    def accept(self, progress: dict[str, Any]) -> bool:
        key = (str(progress.get("stage", "transferring")), progress.get("component_id"))
        now = time.monotonic()
        total = int(progress.get("bytes_total", 0))
        received = int(progress.get("bytes_received", 0))
        permille = int(progress.get("progress_permille", received * 1000 // total if total else 0))
        complete = total > 0 and received >= total
        if key == self._key and not complete and (now - self._updated < 1.0 or permille < self._progress + 10):
            return False
        self._key, self._updated = key, now
        self._progress = permille
        return True
