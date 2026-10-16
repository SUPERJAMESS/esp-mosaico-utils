from __future__ import annotations

import contextlib
import pathlib
import sqlite3
import time
from typing import Any

from .boot_identity import boot_id_text
from .operation_identity import request_fingerprint, require_same_request
from .schema import initialize_schema
from .storage_health import durable_write
from .storage_json import decode_json as _loads
from .storage_json import encode_json as _json


class StateStore:
    """Long-lived identity, configuration and operation evidence."""

    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.db = sqlite3.connect(root / "gateway.sqlite3")
        self.db.row_factory = sqlite3.Row
        try:
            self.schema_version = initialize_schema(self.db)
        except BaseException:
            self.db.close()
            raise
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA foreign_keys=ON")
        self._state_error: str | None = None

    def storage_failed(self, exc: BaseException) -> None:
        with contextlib.suppress(sqlite3.Error):
            self.db.rollback()
        self._state_error = str(exc)

    def close(self) -> None:
        self.db.close()

    def get_setting(self, key: str, default: Any = None) -> Any:
        row = self.db.execute(
            "SELECT value FROM settings WHERE key=?", (key,)
        ).fetchone()
        return _loads(row["value"], default) if row else default

    @durable_write
    def set_setting(self, key: str, value: Any) -> None:
        self.db.execute(
            """INSERT INTO settings(key, value, updated_ns) VALUES(?, ?, ?)
               ON CONFLICT(key) DO UPDATE SET value=excluded.value,
                   updated_ns=excluded.updated_ns""",
            (key, _json(value), time.time_ns()),
        )
        self.db.commit()

    def remember_device(self, info: dict[str, Any], *, commit: bool = True) -> None:
        device_id = str(info["device_id"])
        now = time.time_ns()
        existing = self.db.execute(
            "SELECT cached_json FROM devices WHERE device_id=?", (device_id,)
        ).fetchone()
        cached = _loads(existing["cached_json"], {}) if existing else {}
        cached.update(info)
        self.db.execute(
            """INSERT INTO devices(device_id, first_seen_ns, last_seen_ns, cached_json)
               VALUES(?, ?, ?, ?)
               ON CONFLICT(device_id) DO UPDATE SET
                   last_seen_ns=excluded.last_seen_ns,
                   cached_json=excluded.cached_json""",
            (device_id, now, now, _json(cached)),
        )
        link_infos = [info[key] for key in ("control_link", "data_link") if info.get(key)]
        if not link_infos:
            link_infos = [info]
        for link in link_infos:
            boot_id = link.get("boot_id")
            session_id = link.get("session_id")
            endpoint = link.get("endpoint")
            role = link.get("link_role", "control")
            if session_id is None:
                continue
            active = self.db.execute(
                "SELECT id, boot_id, session_id FROM sessions "
                "WHERE device_id=? AND link_role=? AND ended_ns IS NULL ORDER BY id DESC LIMIT 1",
                (device_id, role),
            ).fetchone()
            if active is None or str(active["session_id"]) != str(session_id) or str(active["boot_id"]) != str(boot_id):
                if active is not None:
                    self.db.execute("UPDATE sessions SET ended_ns=? WHERE id=?", (now, active["id"]))
                self.db.execute(
                    "INSERT INTO sessions(device_id, boot_id, session_id, endpoint, link_role, started_ns) "
                    "VALUES(?, ?, ?, ?, ?, ?)",
                    (device_id, str(boot_id), str(session_id), endpoint, role, now),
                )
        if commit:
            self.db.commit()

    def set_alias(self, device_id: str, alias: str | None) -> None:
        try:
            self.db.execute(
                "UPDATE devices SET alias=? WHERE device_id=?",
                (alias or None, device_id),
            )
        except sqlite3.IntegrityError as exc:
            raise ValueError(f"device alias is already in use: {alias}") from exc
        self.db.commit()

    def remove_device(self, device_id: str) -> bool:
        """Remove a device from inventory without deleting its evidence history."""

        with self.db:
            cursor = self.db.execute(
                "DELETE FROM devices WHERE device_id=?", (device_id,)
            )
            self.db.execute(
                "DELETE FROM settings WHERE key=?", (f"status.{device_id}",)
            )
        return cursor.rowcount > 0

    def cached_devices(self) -> list[dict[str, Any]]:
        rows = self.db.execute(
            "SELECT device_id, alias, last_seen_ns, cached_json FROM devices "
            "ORDER BY COALESCE(alias, device_id)"
        ).fetchall()
        result = []
        for row in rows:
            item = _loads(row["cached_json"], {})
            item.update(
                alias=row["alias"],
                last_seen_ns=row["last_seen_ns"],
                cached=True,
            )
            result.append(item)
        return result

    def resolve_device(self, value: str) -> str:
        row = self.db.execute(
            "SELECT device_id FROM devices WHERE device_id=? OR alias=?", (value, value)
        ).fetchone()
        if row is None:
            raise KeyError(f"unknown device or alias: {value}")
        return str(row["device_id"])

    def preserve_event_evidence(self, event: dict[str, Any], device_id: str | None) -> None:
        """Bind healthy observations to unresolved writes in the caller's transaction."""
        if not device_id or event.get("event_name") != "healthy" or event.get("boot_id") is None:
            return
        self.db.execute(
            """INSERT INTO operation_evidence(operation_id, kind, boot_id, host_receive_ns, payload_json)
               SELECT operation_id, 'healthy', ?, ?, ? FROM operations
               WHERE device_id=? AND created_ns<=?
                 AND action IN ('firmware.ota', 'firmware.system_update', 'host.recovery')
                 AND status NOT IN ('succeeded', 'failed', 'cancelled')
               ON CONFLICT(operation_id, kind, boot_id) DO UPDATE SET
                 host_receive_ns=excluded.host_receive_ns, payload_json=excluded.payload_json
               WHERE excluded.host_receive_ns >= operation_evidence.host_receive_ns""",
            (str(event["boot_id"]), event["host_receive_ns"], _json(event),
             device_id, event["host_receive_ns"]),
        )

    def healthy_evidence(self, operation_id: str, boot_id: Any) -> dict[str, Any] | None:
        row = self.db.execute(
            "SELECT payload_json FROM operation_evidence WHERE operation_id=? AND kind='healthy' AND boot_id=?",
            (operation_id, str(boot_id)),
        ).fetchone()
        return _loads(row[0], None) if row else None

    @durable_write
    def create_operation(self, operation: dict[str, Any]) -> tuple[dict[str, Any], bool]:
        fingerprint = operation.get("request_fingerprint") or request_fingerprint(operation)
        cursor = self.db.execute(
            """INSERT INTO operations(
                   operation_id, device_id, actor_type, actor_name, action,
                   params_json, status, created_ns, queue_position, request_fingerprint)
               VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT(operation_id) DO NOTHING""",
            (
                operation["operation_id"],
                operation["device_id"],
                operation["actor_type"],
                operation["actor_name"],
                operation["action"],
                _json(operation.get("params", {})),
                operation["status"],
                operation["created_ns"],
                operation.get("queue_position", 0),
                fingerprint,
            ),
        )
        self.db.commit()
        stored = self.operation(str(operation["operation_id"]))
        assert stored is not None
        require_same_request(stored, fingerprint)
        return stored, cursor.rowcount == 1

    @durable_write
    def update_operation(self, operation_id: str, **changes: Any) -> dict[str, Any]:
        columns = {
            "status",
            "result_json",
            "error",
            "started_ns",
            "finished_ns",
            "queue_position",
            "progress_json",
        }
        updates = []
        values: list[Any] = []
        for key, value in changes.items():
            if key not in columns:
                continue
            updates.append(f"{key}=?")
            values.append(
                _json(value) if key in {"result_json", "progress_json"} else value
            )
        if updates:
            values.append(operation_id)
            self.db.execute(
                f"UPDATE operations SET {', '.join(updates)} WHERE operation_id=?",
                values,
            )
            self.db.commit()
        item = self.operation(operation_id)
        if item is None:
            raise KeyError(operation_id)
        return item

    def operation(self, operation_id: str) -> dict[str, Any] | None:
        row = self.db.execute(
            "SELECT * FROM operations WHERE operation_id=?", (operation_id,)
        ).fetchone()
        return self._operation_row(row) if row else None

    def operations(
        self, device_id: str | None = None, limit: int = 200
    ) -> list[dict[str, Any]]:
        if device_id:
            rows = self.db.execute(
                "SELECT * FROM operations WHERE device_id=? "
                "ORDER BY created_ns DESC LIMIT ?",
                (device_id, limit),
            ).fetchall()
        else:
            rows = self.db.execute(
                "SELECT * FROM operations ORDER BY created_ns DESC LIMIT ?", (limit,)
            ).fetchall()
        return [self._operation_row(row) for row in rows]

    @staticmethod
    def _operation_row(row: sqlite3.Row) -> dict[str, Any]:
        item = dict(row)
        item["params"] = _loads(item.pop("params_json"), {})
        item["result"] = _loads(item.pop("result_json"), None)
        item["progress"] = _loads(item.pop("progress_json", None), None)
        display = boot_id_text(item)
        display["params"] = item["params"]
        return display

    def add_audit(
        self,
        actor_type: str,
        actor_name: str,
        action: str,
        details: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        now = time.time_ns()
        cursor = self.db.execute(
            "INSERT INTO system_audit(actor_type, actor_name, action, details_json, created_ns) "
            "VALUES(?, ?, ?, ?, ?)",
            (actor_type, actor_name, action, _json(details or {}), now),
        )
        self.db.commit()
        if cursor.lastrowid is None:
            raise RuntimeError("SQLite did not return an audit ID")
        return {
            "audit_id": int(cursor.lastrowid),
            "actor_type": actor_type,
            "actor_name": actor_name,
            "action": action,
            "details": details or {},
            "created_ns": now,
        }

    def audits(self, limit: int = 200) -> list[dict[str, Any]]:
        rows = self.db.execute(
            "SELECT * FROM system_audit ORDER BY audit_id DESC LIMIT ?", (limit,)
        ).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            item["details"] = _loads(item.pop("details_json"), {})
            result.append(boot_id_text(item))
        return result
