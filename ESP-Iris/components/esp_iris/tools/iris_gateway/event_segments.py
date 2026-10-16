"""Rotating event databases with durable allocation and recoverable retirement."""
from __future__ import annotations

import contextlib
import pathlib
import sqlite3
import threading
import time
import weakref
from collections.abc import Iterable
from typing import Any

from .retention import RetentionPolicy
from .state_store import StateStore
from .storage_json import decode_json, encode_json

_LOCKS: Any = weakref.WeakValueDictionary()
_LOCKS_GUARD = threading.Lock()
_SCHEMA = """
    CREATE TABLE events (event_id INTEGER PRIMARY KEY, device_id TEXT,
        category TEXT NOT NULL, host_receive_ns INTEGER NOT NULL,
        payload_json TEXT NOT NULL, capture_id TEXT);
    CREATE INDEX events_device ON events(device_id, event_id);
    CREATE INDEX events_capture ON events(capture_id, event_id);
    PRAGMA user_version=1;
"""


class EventSegments:
    def __init__(self, state: StateStore) -> None:
        self.state, self.db = state, state.db
        self.root = state.root / "events"
        self.root.mkdir(exist_ok=True)
        with _LOCKS_GUARD:
            key = str(self.root.resolve())
            self.lock = _LOCKS.get(key)
            if self.lock is None:
                self.lock = threading.RLock()
                _LOCKS[key] = self.lock

    def path(self, start: int) -> pathlib.Path:
        return self.root / f"segment-{start:020d}.sqlite3"

    def files(self, start: int) -> list[pathlib.Path]:
        path = self.path(start)
        return [path, pathlib.Path(str(path) + "-wal"), pathlib.Path(str(path) + "-shm")]

    def size(self, start: int) -> int:
        return sum(path.stat().st_size for path in self.files(start) if path.exists())

    def _setting(self, key: str, value: Any) -> None:
        self.db.execute("INSERT INTO settings(key,value,updated_ns) VALUES(?,?,?) "
                        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_ns=excluded.updated_ns",
                        (key, encode_json(value), time.time_ns()))

    def recover_pending(self) -> None:
        """State commits first. An interrupted event commit becomes an explicit gap."""
        pending = self.state.get_setting("events.pending")
        if not pending:
            return
        path = self.path(pending["segment"])
        present: set[int] = set()
        if path.exists():
            with contextlib.closing(sqlite3.connect(path)) as db:
                present = {row[0] for row in db.execute("SELECT event_id FROM events WHERE event_id BETWEEN ? AND ?",
                                                       (pending["start"], pending["end"]))}
        for event_id in range(pending["start"], pending["end"] + 1):
            if event_id not in present:
                self.db.execute("INSERT OR IGNORE INTO event_gaps VALUES(?,?,?)",
                                (event_id, event_id, "interrupted event commit"))
        self.db.execute("DELETE FROM settings WHERE key='events.pending'")
        self.db.commit()

    def append(self, rows: list[tuple[str, dict[str, Any], str | None, int]]) -> list[dict[str, Any]]:
        """Commit evidence/catalog before history; never assume cross-file atomicity."""
        policy = RetentionPolicy(**self.state.get_setting("storage.retention", {}))
        current = self.db.execute("SELECT * FROM event_segments WHERE status='active'").fetchone()
        now = time.time_ns()
        if current and (self.size(current["start_id"]) >= policy.segment_bytes
                        or now - current["created_ns"] >= policy.segment_seconds * 1_000_000_000):
            self.db.execute("UPDATE event_segments SET status='closed' WHERE start_id=?", (current["start_id"],))
            current = None
        sequence = self.state.get_setting("events.sequence")
        if sequence is None:
            old = self.db.execute("SELECT seq FROM sqlite_sequence WHERE name='events'").fetchone()
            sequence = int(old[0]) if old else 0
        first = int(sequence) + 1
        start = current["start_id"] if current else first
        path = self.path(start)
        if current and not path.exists():
            raise OSError("active event segment is missing; history must be inspected before writing")
        with contextlib.closing(sqlite3.connect(path)) as db:
            if not db.execute("PRAGMA user_version").fetchone()[0]:
                db.executescript(_SCHEMA)
            if db.execute("PRAGMA user_version").fetchone()[0] != 1:
                raise ValueError("unsupported event segment schema")
            if current is None:
                if db.execute("SELECT 1 FROM events LIMIT 1").fetchone():
                    raise RuntimeError("unregistered nonempty event segment; refusing to overwrite history")
                self.db.execute("INSERT INTO event_segments VALUES(?,?,?,?,0,'active')", (start, first - 1, now, now))
            result = []
            for offset, (category, payload, device_id, host_ns) in enumerate(rows):
                event_id = first + offset
                item = dict(payload, event_id=event_id, category=category, host_receive_ns=host_ns)
                result.append(item)
                db.execute("INSERT INTO events VALUES(?,?,?,?,?,?)",
                           (event_id, device_id, category, host_ns, encode_json(payload), payload.get("capture_id")))
            last = first + len(rows) - 1
            self.db.execute("UPDATE event_segments SET last_id=?,last_ns=?,row_count=row_count+? WHERE start_id=?",
                            (last, rows[-1][3], len(rows), start))
            self._setting("events.sequence", last)
            self._setting("events.pending", {"segment": start, "start": first, "end": last})
            self.db.commit()
            db.commit()
        # The reservation remains until the next write/recovery, so a crash
        # between the two commits is detectable without per-event acknowledgement.
        return result

    def query(self, *, cursor: int = 0, descending: bool = False, device_id: str | None = None,
              categories: Iterable[str] | None = None, capture_id: str | None = None,
              endpoint: str | None = None, limit: int = 1000,
              unresolved: bool = False) -> tuple[list[dict[str, Any]], bool]:
        with self.lock:
            segments = [dict(row) for row in self.db.execute(
                "SELECT * FROM event_segments WHERE status!='deleting' ORDER BY start_id")]
            sources = [(self.state.root / "gateway.sqlite3", True)] + [(self.path(row["start_id"]), False) for row in segments]
            if descending:
                sources.reverse()
            remaining = max(1, min(limit, 5000))
            remaining_bytes = 8 * 1024 * 1024
            items: list[dict[str, Any]] = []
            missing = False
            category_values = list(categories or ())
            for path, legacy in sources:
                if not path.exists():
                    missing = True
                    continue
                with contextlib.closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as db:
                    db.row_factory = sqlite3.Row
                    schema = "main" if legacy else "state"
                    if not legacy:
                        db.execute("ATTACH DATABASE ? AS state", ((self.state.root / "gateway.sqlite3").as_uri() + "?mode=ro",))
                    resolved = f"COALESCE(e.device_id,(SELECT device_id FROM {schema}.console_captures c WHERE c.capture_id=e.capture_id))"
                    clauses = ["e.event_id> ?"]
                    values: list[int | str] = [cursor]
                    if unresolved:
                        clauses.append(resolved + " IS NULL")
                    if device_id:
                        clauses.append(resolved + "=?")
                        values.append(device_id)
                    if capture_id:
                        clauses.append("e.capture_id=?")
                        values.append(capture_id)
                    if endpoint:
                        clauses.append(f"e.capture_id IN (SELECT capture_id FROM {schema}.console_captures WHERE endpoint=?)")
                        values.append(endpoint)
                    if category_values:
                        clauses.append("e.category IN (" + ",".join("?" for _ in category_values) + ")")
                        values.extend(category_values)
                    order = "DESC" if descending else "ASC"
                    rows = db.execute("SELECT e.event_id,e.category,e.host_receive_ns, "
                                      "CASE WHEN length(CAST(e.payload_json AS BLOB))<=8388096 THEN e.payload_json ELSE NULL END AS payload_json, "
                                      + resolved + " AS resolved_device FROM events e WHERE " +
                                      " AND ".join(clauses) + f" ORDER BY event_id {order} LIMIT ?", [*values, remaining])
                    for row in rows:
                        payload = row["payload_json"]
                        amount = len(payload.encode()) + 256 if payload is not None else 256
                        if items and amount > remaining_bytes:
                            remaining = 0
                            break
                        item = decode_json(payload, {"kind": "history_gap", "reason": "legacy event exceeds query byte budget",
                                                     "dropped_events": 1})
                        remaining_bytes -= min(amount, remaining_bytes)
                        if row["resolved_device"] is not None:
                            item["device_id"] = row["resolved_device"]
                        item.update(event_id=row["event_id"], category=row["category"], host_receive_ns=row["host_receive_ns"])
                        items.append(item)
                        remaining -= 1
                if not remaining:
                    break
            through = max((item["event_id"] for item in items), default=int(self.state.get_setting("events.sequence", 0)))
            gap = missing or bool(cursor and cursor < int(self.state.get_setting("events.pruned_through", 0)))
            gap = gap or bool(self.db.execute("SELECT 1 FROM event_gaps WHERE end_id>? AND start_id<=? LIMIT 1",
                                             (cursor, through)).fetchone())
            return (list(reversed(items)) if descending else items), gap

    def prune(self, policy: RetentionPolicy, *, now_ns: int | None = None) -> int:
        with self.lock:
            self.recover_pending()
            rows = [dict(row) for row in self.db.execute("SELECT * FROM event_segments ORDER BY start_id")]
            total_bytes = sum(self.size(row["start_id"]) for row in rows)
            total_rows = sum(row["row_count"] for row in rows)
            cutoff = (time.time_ns() if now_ns is None else now_ns) - policy.event_days * 86_400_000_000_000
            removed = 0
            for index, row in enumerate(rows[:4]):
                if (row["status"] != "deleting" and row["last_ns"] >= cutoff and total_bytes <= policy.event_bytes
                        and total_rows <= policy.event_rows and len(rows) - index <= policy.segment_files):
                    break
                start, end = row["start_id"], row["last_id"]
                size = self.size(start)
                # Publish the retirement boundary before unlinking any file.
                with self.db:
                    self.db.execute("UPDATE event_segments SET status='deleting' WHERE start_id=?", (start,))
                    self._setting("events.pruned_through", max(end, int(self.state.get_setting("events.pruned_through", 0))))
                for path in self.files(start):
                    path.unlink(missing_ok=True)
                with self.db:
                    self.db.execute("DELETE FROM event_segments WHERE start_id=?", (start,))
                    self.db.execute("DELETE FROM event_gaps WHERE end_id<=?", (end,))
                removed += row["row_count"]
                total_rows -= row["row_count"]
                total_bytes -= size
            return removed
