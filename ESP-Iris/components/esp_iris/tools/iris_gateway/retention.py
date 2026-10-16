"""Bounded maintenance of disposable diagnostics, never device evidence."""
from __future__ import annotations

import dataclasses
import pathlib
import sqlite3
import time
from typing import Any


@dataclasses.dataclass(frozen=True)
class RetentionPolicy:
    event_days: int = 7
    event_rows: int = 200_000
    event_bytes: int = 512 * 1024 * 1024
    raw_days: int = 7
    raw_bytes: int = 256 * 1024 * 1024
    batch_rows: int = 512
    segment_bytes: int = 8 * 1024 * 1024
    segment_seconds: int = 3600
    segment_files: int = 64

    def __post_init__(self) -> None:
        for field in dataclasses.fields(self):
            value = getattr(self, field.name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"{field.name} must be a non-negative integer")
        if not 1 <= self.batch_rows <= 4096:
            raise ValueError("batch_rows must be 1..4096")
        if self.segment_bytes < 16384 or self.segment_seconds < 1 or not 1 <= self.segment_files <= 1024:
            raise ValueError("segments require at least 16 KiB, 1 second and 1..1024 files")


def disk_usage(root: pathlib.Path) -> dict[str, int]:
    database = root / "gateway.sqlite3"
    result = {name: path.stat().st_size if path.exists() else 0 for name, path in (
        ("database_bytes", database), ("wal_bytes", pathlib.Path(str(database) + "-wal")),
        ("shm_bytes", pathlib.Path(str(database) + "-shm")))}
    result["event_segment_bytes"] = sum(path.stat().st_size for path in (root / "events").glob("segment-*") if path.is_file())
    return result


def prune_events(store: Any, policy: RetentionPolicy, *, now_ns: int | None = None) -> int:
    """One bounded prefix transaction. Existing 0.2 evidence is protected first."""
    db = store.db
    boundary = db.execute("SELECT event_id FROM events ORDER BY event_id DESC LIMIT 1 OFFSET ?",
                          (policy.event_rows,)).fetchone()
    row_cutoff = int(boundary[0]) if boundary else 0
    page_size = int(db.execute("PRAGMA page_size").fetchone()[0])
    used_bytes = (int(db.execute("PRAGMA page_count").fetchone()[0]) -
                  int(db.execute("PRAGMA freelist_count").fetchone()[0])) * page_size
    cutoff = (now_ns if now_ns is not None else time.time_ns()) - policy.event_days * 86_400_000_000_000
    rows = db.execute("SELECT * FROM events ORDER BY event_id LIMIT ?", (policy.batch_rows,)).fetchall()
    selected = []
    for row in rows:
        if row["event_id"] <= row_cutoff or row["host_receive_ns"] < cutoff or used_bytes > policy.event_bytes:
            selected.append(row)
        else:
            break
    if not selected:
        return 0
    import json

    with db:
        for row in selected:
            if row["category"] == "device":
                event = dict(json.loads(row["payload_json"]), host_receive_ns=row["host_receive_ns"])
                store.preserve_event_evidence(event, row["device_id"])
        last = int(selected[-1]["event_id"])
        db.execute("DELETE FROM events WHERE event_id<=?", (last,))
        db.execute("INSERT INTO settings(key,value,updated_ns) VALUES('events.pruned_through',?,?) "
                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_ns=excluded.updated_ns",
                   (str(max(last, int(store.get_setting("events.pruned_through", 0)))), time.time_ns()))
    return len(selected)


def maintain(root: pathlib.Path) -> dict[str, Any]:
    # The worker owns its connection; the event-loop connection is never shared.
    from .store import GatewayStore

    store = GatewayStore(root)
    try:
        store.db.execute("PRAGMA busy_timeout=100")
        policy = RetentionPolicy(**store.get_setting("storage.retention", {}))
        deadline = time.monotonic() + 0.05
        removed = 0
        while time.monotonic() < deadline:
            count = prune_events(store, policy)
            removed += count
            if not count:
                break
        removed += store.events.segments.prune(policy)
        store.cleanup_raw_logs(policy.raw_days, policy.raw_bytes)
        store.db.execute("PRAGMA wal_checkpoint(PASSIVE)")
        store.db.execute("PRAGMA incremental_vacuum(128)")
        return {"status": "ok", "removed_events": removed, **disk_usage(root)}
    except (sqlite3.Error, OSError) as exc:
        return {"status": "degraded", "error": str(exc)}
    finally:
        store.close()
