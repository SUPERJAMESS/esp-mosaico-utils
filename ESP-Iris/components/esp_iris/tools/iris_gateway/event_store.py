from __future__ import annotations

import base64
import pathlib
import re
import sqlite3
import time
from collections import OrderedDict
from collections.abc import Iterable
from typing import Any

from .event_segments import EventSegments
from .state_store import StateStore


class EventStore:
    """Disposable segmented history; identity bindings remain in durable state."""

    def __init__(self, state: StateStore, root: pathlib.Path) -> None:
        self.state, self.root, self.db = state, root, state.db
        self.logs_dir = root / "logs"
        self.logs_dir.mkdir(exist_ok=True)
        self._lost_captures: OrderedDict[str, tuple[int, int]] = OrderedDict()
        self.segments = EventSegments(state)
        self.recovery_error: str | None = None
        try:
            with self.segments.lock:
                self.segments.recover_pending()
        except (sqlite3.Error, OSError) as exc:
            self.recovery_error = str(exc)

    def append_event(self, category: str, payload: dict[str, Any],
                     device_id: str | None = None) -> dict[str, Any]:
        return self.append_batch([(category, payload, device_id)])[-1]

    def append_batch(self, events: list[tuple[str, dict[str, Any], str | None]],
                     devices: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
        with self.segments.lock:
            self.segments.recover_pending()
            try:
                for device in devices or []:
                    self.state.remember_device(device, commit=False)
                rows = []
                for category, payload, device_id in events:
                    rows.extend(self.prepare_event(category, payload, device_id))
                result = self.segments.append(rows)
            except BaseException:
                self.db.rollback()
                raise
            for _, payload, _ in events:
                if payload.get("kind") == "console_raw":
                    capture = str(payload.get("capture_id"))
                    lost = self._lost_captures.get(capture)
                    if lost and int(payload["offset"]) >= lost[1]:
                        self._lost_captures.pop(capture, None)
            return result

    def prepare_event(self, category: str, payload: dict[str, Any],
                      device_id: str | None) -> list[tuple[str, dict[str, Any], str | None, int]]:
        host_ns = int(payload.get("host_receive_wall_ns") or time.time_ns())
        rows: list[tuple[str, dict[str, Any], str | None, int]] = []
        if payload.get("kind") == "connection" and payload.get("connection_state") == "disconnected":
            self.db.execute("UPDATE sessions SET ended_ns=? WHERE device_id=? AND endpoint=? AND session_id=? AND ended_ns IS NULL",
                            (host_ns, device_id, payload.get("endpoint"), str(payload.get("session_id"))))
        capture_id = payload.get("capture_id")
        if capture_id is not None and not re.fullmatch(r"[0-9a-f]{32}", str(capture_id)):
            raise ValueError("invalid console capture identity")
        if payload.get("kind") == "console_raw":
            raw = base64.b64decode(payload["data_base64"], validate=True)
            offset = int(payload["offset"])
            row = self.db.execute("SELECT bytes_received FROM console_captures WHERE capture_id=?", (capture_id,)).fetchone()
            expected = int(row[0]) if row else 0
            lost = self._lost_captures.get(str(capture_id))
            if offset != expected and (not lost or offset <= expected or lost[1] != offset):
                raise ValueError("raw console capture contains an offset gap")
            if lost and lost[1] == offset:
                gap = dict(payload, kind="console_gap", offset=min(expected, lost[0]), end_offset=offset,
                           reason="event storage unavailable")
                gap.pop("data_base64", None)
                rows.append(("console", gap, device_id, host_ns))
            self.db.execute(
                "INSERT INTO console_captures(capture_id, endpoint, device_id, first_ns, last_ns, bytes_received) "
                "VALUES(?, ?, ?, ?, ?, ?) ON CONFLICT(capture_id) DO UPDATE SET "
                "last_ns=excluded.last_ns, bytes_received=excluded.bytes_received, "
                "device_id=COALESCE(excluded.device_id, console_captures.device_id)",
                (capture_id, payload["endpoint"], device_id, host_ns, host_ns, offset + len(raw)))
        elif payload.get("kind") == "console_binding" and device_id:
            first, _ = self.segments.query(capture_id=str(capture_id), limit=1, unresolved=True)
            last, _ = self.segments.query(capture_id=str(capture_id), descending=True, limit=1, unresolved=True)
            if first:
                payload = dict(payload, history_start_event_id=first[0]["event_id"], history_end_event_id=last[-1]["event_id"])
            self.db.execute(
                "INSERT INTO console_captures(capture_id,endpoint,device_id,first_ns,last_ns,bytes_received,binding_offset) "
                "VALUES(?,?,?,?,?,?,?) ON CONFLICT(capture_id) DO UPDATE SET "
                "device_id=excluded.device_id, binding_offset=excluded.binding_offset",
                (capture_id, payload.get("endpoint", ""), device_id,
                 first[0]["host_receive_ns"] if first else host_ns, host_ns,
                 int(payload["offset"]), int(payload["offset"])))
        self.state.preserve_event_evidence(dict(payload, host_receive_ns=host_ns), device_id)
        rows.append((category, payload, device_id, host_ns))
        return rows

    def events_after(self, cursor: int, *, device_id: str | None = None,
                     categories: Iterable[str] | None = None, capture_id: str | None = None,
                     endpoint: str | None = None, limit: int = 1000) -> tuple[list[dict[str, Any]], bool]:
        return self.segments.query(cursor=cursor, device_id=device_id, categories=categories,
                                   capture_id=capture_id, endpoint=endpoint, limit=limit)

    def latest_events(self, *, device_id: str | None = None, categories: Iterable[str] | None = None,
                      capture_id: str | None = None, endpoint: str | None = None,
                      limit: int = 1000) -> list[dict[str, Any]]:
        return self.segments.query(descending=True, device_id=device_id, categories=categories,
                                   capture_id=capture_id, endpoint=endpoint, limit=limit)[0]

    def cleanup_raw_logs(
        self, max_age_days: int = 7, max_bytes: int = 1024 * 1024 * 1024
    ) -> None:
        files = sorted(
            (path for path in self.logs_dir.rglob("*.jsonl.gz") if path.is_file()),
            key=lambda path: path.stat().st_mtime,
        )
        cutoff = time.time() - max_age_days * 86400
        for path in list(files):
            if path.stat().st_mtime < cutoff:
                path.unlink(missing_ok=True)
                files.remove(path)
        total = sum(path.stat().st_size for path in files)
        while files and total > max_bytes:
            path = files.pop(0)
            total -= path.stat().st_size
            path.unlink(missing_ok=True)
        for row in self.db.execute("SELECT id, path FROM log_index").fetchall():
            if not (self.root / row["path"]).is_file():
                self.db.execute("DELETE FROM log_index WHERE id=?", (row["id"],))
        self.db.commit()
