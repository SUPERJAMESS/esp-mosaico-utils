from __future__ import annotations

import base64
import gzip
import pathlib
import re
import time
from collections import OrderedDict
from collections.abc import Iterable
from typing import Any

from .state_store import StateStore
from .storage_json import decode_json as _loads
from .storage_json import encode_json as _json


class EventStore:
    """Disposable event history and raw diagnostic logs."""

    def __init__(self, state: StateStore, root: pathlib.Path) -> None:
        self.state, self.root, self.db = state, root, state.db
        self.logs_dir = root / "logs"
        self.logs_dir.mkdir(exist_ok=True)
        self._lost_captures: OrderedDict[str, int] = OrderedDict()

    def append_event(
        self, category: str, payload: dict[str, Any], device_id: str | None = None
    ) -> dict[str, Any]:
        host_ns = int(payload.get("host_receive_wall_ns") or time.time_ns())
        if payload.get("kind") == "connection" and payload.get("connection_state") == "disconnected":
            self.db.execute("UPDATE sessions SET ended_ns=? WHERE device_id=? AND endpoint=? AND session_id=? AND ended_ns IS NULL",
                (host_ns, device_id, payload.get("endpoint"), str(payload.get("session_id"))))
        capture_id = payload.get("capture_id")
        if capture_id is not None and not re.fullmatch(r"[0-9a-f]{32}", str(capture_id)):
            raise ValueError("invalid console capture identity")
        if payload.get("kind") == "console_raw":
            raw = base64.b64decode(payload["data_base64"], validate=True)
            offset = int(payload["offset"])
            row = self.db.execute("SELECT bytes_received FROM console_captures WHERE capture_id=?",
                                  (capture_id,)).fetchone()
            expected = int(row[0]) if row else 0
            if offset != expected:
                if offset <= expected or self._lost_captures.get(str(capture_id)) != offset:
                    raise ValueError("raw console capture contains an offset gap")
                gap = dict(payload, kind="console_gap", offset=expected, end_offset=offset,
                           reason="event storage unavailable")
                gap.pop("data_base64", None)
                self.db.execute(
                    "INSERT INTO events(device_id, category, host_receive_ns, payload_json, capture_id) "
                    "VALUES(?, 'console', ?, ?, ?)", (device_id, host_ns, _json(gap), capture_id))
            self.db.execute(
                "INSERT INTO console_captures(capture_id, endpoint, device_id, first_ns, last_ns, bytes_received) "
                "VALUES(?, ?, ?, ?, ?, ?) ON CONFLICT(capture_id) DO UPDATE SET "
                "last_ns=excluded.last_ns, bytes_received=excluded.bytes_received, "
                "device_id=COALESCE(excluded.device_id, console_captures.device_id)",
                (capture_id, payload["endpoint"], device_id, host_ns, host_ns, offset + len(raw)))
        elif payload.get("kind") == "console_binding" and device_id:
            pending = self.db.execute(
                "SELECT MIN(event_id), MAX(event_id) FROM events "
                "WHERE capture_id=? AND device_id IS NULL", (capture_id,)
            ).fetchone()
            if pending[0] is not None:
                payload = dict(payload, history_start_event_id=int(pending[0]),
                               history_end_event_id=int(pending[1]))
            self.db.execute("UPDATE console_captures SET device_id=?, binding_offset=? WHERE capture_id=?",
                            (device_id, int(payload["offset"]), capture_id))
            # The later HELLO associates the capture with a device. It cannot
            # establish which boot produced earlier bytes, so boot_id stays NULL.
            self.db.execute("UPDATE events SET device_id=? WHERE capture_id=? AND device_id IS NULL",
                            (device_id, capture_id))
        cursor = self.db.execute(
            "INSERT INTO events(device_id, category, host_receive_ns, payload_json, capture_id) "
            "VALUES(?, ?, ?, ?, ?)",
            (device_id, category, host_ns, _json(payload), capture_id),
        )
        if cursor.lastrowid is None:
            raise RuntimeError("SQLite did not return an event ID")
        event_id = int(cursor.lastrowid)
        self.state.preserve_event_evidence(dict(payload, event_id=event_id, category=category,
                                         host_receive_ns=host_ns), device_id)
        self.db.commit()
        if capture_id and payload.get("kind") == "console_raw":
            self._lost_captures.pop(str(capture_id), None)
        item = dict(payload)
        item.update(event_id=event_id, category=category, host_receive_ns=host_ns)
        if capture_id and payload.get("kind") in {"console_raw", "console_binding", "console_gap", "hardware_reset"}:
            self._append_raw_log("capture-" + capture_id, event_id, host_ns, item)
        elif category == "log" and device_id:
            self._append_raw_log(device_id, event_id, host_ns, item)
        return item

    def events_after(
        self,
        cursor: int,
        *,
        device_id: str | None = None,
        categories: Iterable[str] | None = None,
        capture_id: str | None = None,
        endpoint: str | None = None,
        limit: int = 1000,
    ) -> tuple[list[dict[str, Any]], bool]:
        oldest_row = self.db.execute("SELECT MIN(event_id) AS value FROM events").fetchone()
        oldest = int(oldest_row["value"] or 0)
        history_gap = bool(cursor and oldest and cursor < oldest - 1)
        history_gap = history_gap or bool(cursor and cursor < int(self.state.get_setting("events.pruned_through", 0)))
        clauses = ["event_id > ?"]
        values: list[Any] = [cursor]
        if device_id:
            clauses.append("device_id=?")
            values.append(device_id)
        if capture_id:
            clauses.append("capture_id=?")
            values.append(capture_id)
        if endpoint:
            clauses.append("capture_id IN (SELECT capture_id FROM console_captures WHERE endpoint=?)")
            values.append(endpoint)
        category_values = list(categories or ())
        if category_values:
            clauses.append(
                f"category IN ({','.join('?' * len(category_values))})"
            )
            values.extend(category_values)
        values.append(max(1, min(limit, 5000)))
        rows = self.db.execute(
            "SELECT * FROM events WHERE " + " AND ".join(clauses)
            + " ORDER BY event_id LIMIT ?",
            values,
        ).fetchall()
        items: list[dict[str, Any]] = []
        for row in rows:
            item = _loads(row["payload_json"], {})
            if row["device_id"] is not None:
                item["device_id"] = row["device_id"]
            item.update(
                event_id=row["event_id"],
                category=row["category"],
                host_receive_ns=row["host_receive_ns"],
            )
            items.append(item)
        return items, history_gap

    def latest_events(
        self,
        *,
        device_id: str | None = None,
        categories: Iterable[str] | None = None,
        capture_id: str | None = None,
        endpoint: str | None = None,
        limit: int = 1000,
    ) -> list[dict[str, Any]]:
        clauses: list[str] = []
        values: list[Any] = []
        if device_id:
            clauses.append("device_id=?")
            values.append(device_id)
        if capture_id:
            clauses.append("capture_id=?")
            values.append(capture_id)
        if endpoint:
            clauses.append("capture_id IN (SELECT capture_id FROM console_captures WHERE endpoint=?)")
            values.append(endpoint)
        category_values = list(categories or ())
        if category_values:
            clauses.append(
                f"category IN ({','.join('?' * len(category_values))})"
            )
            values.extend(category_values)
        values.append(max(1, min(limit, 5000)))
        where = " WHERE " + " AND ".join(clauses) if clauses else ""
        rows = self.db.execute(
            "SELECT * FROM events" + where + " ORDER BY event_id DESC LIMIT ?",
            values,
        ).fetchall()
        items: list[dict[str, Any]] = []
        for row in reversed(rows):
            item = _loads(row["payload_json"], {})
            if row["device_id"] is not None:
                item["device_id"] = row["device_id"]
            item.update(
                event_id=row["event_id"],
                category=row["category"],
                host_receive_ns=row["host_receive_ns"],
            )
            items.append(item)
        return items

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

    def _append_raw_log(
        self, device_id: str, event_id: int, host_ns: int, payload: dict[str, Any]
    ) -> None:
        day = time.strftime("%Y-%m-%d", time.localtime(host_ns / 1_000_000_000))
        folder = self.logs_dir / device_id
        folder.mkdir(exist_ok=True)
        path = folder / f"{day}.jsonl.gz"
        with gzip.open(path, "at", encoding="utf-8") as handle:
            handle.write(_json(payload) + "\n")
        relative = path.relative_to(self.root).as_posix()
        size = path.stat().st_size
        self.db.execute(
            """INSERT INTO log_index(
                   device_id, path, first_event_id, last_event_id,
                   first_ns, last_ns, compressed_bytes)
               VALUES(?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT(device_id, path) DO UPDATE SET
                   last_event_id=excluded.last_event_id,
                   last_ns=excluded.last_ns,
                   compressed_bytes=excluded.compressed_bytes""",
            (device_id, relative, event_id, event_id, host_ns, host_ns, size),
        )
        self.db.commit()


