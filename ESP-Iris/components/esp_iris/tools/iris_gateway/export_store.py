"""Bounded evidence snapshots and streaming ZIP creation (worker-thread only)."""
from __future__ import annotations

import contextlib
import hashlib
import os
import pathlib
import shutil
import sqlite3
import tempfile
import threading
import time
import uuid
import zipfile
from collections.abc import Callable, Iterable
from typing import Any

from .artifact_store import ArtifactStore
from .event_store import EventStore
from .state_store import StateStore
from .storage_json import decode_json, encode_json

CHUNK = 64 * 1024


class ExportCancelled(RuntimeError):
    pass


def export_scope(value: dict[str, Any]) -> dict[str, Any]:
    allowed = {"device_id", "operation_id", "since_ns", "until_ns", "retain"}
    if set(value) - allowed:
        raise ValueError("unknown export options")
    result = {key: item for key, item in value.items() if item is not None and item != ""}
    for key in ("device_id", "operation_id"):
        if key in result and (not isinstance(result[key], str) or len(result[key]) > 256):
            raise ValueError(f"invalid {key}")
    for key in ("since_ns", "until_ns"):
        if key in result and (type(result[key]) is not int or not 0 <= result[key] <= 2**63 - 1):
            raise ValueError(f"invalid {key}")
    if "retain" in result and type(result["retain"]) is not bool:
        raise ValueError("retain must be boolean")
    if result.get("since_ns", 0) > result.get("until_ns", 2**63 - 1):
        raise ValueError("since_ns must not exceed until_ns")
    return result


class ExportBudget:
    def __init__(self, directory: pathlib.Path, maximum: int, cancel: threading.Event,
                 progress: Callable[..., None], timeout: int = 300) -> None:
        self.directory, self.maximum, self.cancel, self.progress = directory, maximum, cancel, progress
        self.deadline = time.monotonic() + timeout
        self.input_bytes = self.files = 0
        self.archive: pathlib.Path | None = None
        self.snapshot_bytes = 0
        self._next_disk_check = 0

    def check(self, *, extra: int = 0, disk: bool = False) -> None:
        if self.cancel.is_set():
            raise ExportCancelled("export cancelled")
        if time.monotonic() > self.deadline:
            raise TimeoutError("export exceeded its time budget; narrow the scope")
        if self.input_bytes + extra > self.maximum:
            raise OSError("export exceeds its byte budget; narrow the scope")
        if disk:
            size = sum(path.stat().st_size for path in self.directory.rglob("*") if path.is_file())
            if self.archive is not None and self.archive.exists():
                size += self.archive.stat().st_size
            if size + extra > self.maximum:
                raise OSError("export temporary disk budget exceeded; narrow the scope")
            if shutil.disk_usage(self.directory).free < extra + 8 * 1024 * 1024:
                raise OSError("insufficient free space for export")


class ExportStore:
    def __init__(self, state: StateStore, events: EventStore, artifacts: ArtifactStore) -> None:
        self.state, self.root, self.db = state, state.root, state.db
        self.logs_dir, self.artifacts_dir = events.logs_dir, artifacts.artifacts_dir
        self.events = events

    def _snapshot(self, folder: pathlib.Path, budget: ExportBudget) -> pathlib.Path:
        """Seal and hard-link segments briefly; back up the state read snapshot unlocked.

        Rotation/deletion uses the same lock. Hard links keep closed segments alive
        through retention without copying them or holding up the writer during ZIP.
        No credentials database or raw state snapshot is included in the bundle.
        """
        segments = folder / "segments"
        segments.mkdir()
        try:
            with self.events.segments.lock:
                self.events.segments.recover_pending()
                self.db.execute("UPDATE event_segments SET status='closed' WHERE status='active'")
                self.db.commit()
                self.db.execute("BEGIN")
                rows = list(self.db.execute("SELECT start_id FROM event_segments WHERE status!='deleting' ORDER BY start_id"))
                sources = [self.events.segments.path(row[0]) for row in rows]
                sources = [source for source in sources if source.exists()]
                budget.check(extra=sum(source.stat().st_size for source in sources), disk=True)
                for source in sources:
                    budget.check()
                    os.link(source, segments / source.name)
            target = folder / "state.sqlite3"
            with contextlib.closing(sqlite3.connect(target)) as snapshot:
                def backup_progress(status, remaining, total):
                    budget.check(extra=256 * 1024, disk=True)
                self.db.backup(snapshot, pages=64, progress=backup_progress, sleep=.01)
            return target
        finally:
            self.db.rollback()

    def export_zip(self, target: pathlib.Path | None = None, *, scope: dict[str, Any] | None = None,
                   cancel: threading.Event | None = None, maximum: int = 1024**3,
                   progress: Callable[..., None] = lambda **changes: None, timeout: int = 300) -> pathlib.Path:
        scope = export_scope(scope or {})
        target = target or self.root / f"esp-iris-export-{uuid.uuid4().hex}.zip"
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise FileExistsError(target)
        temporary = target.with_suffix(".partial")
        try:
            with tempfile.TemporaryDirectory(prefix="snapshot-", dir=target.parent) as name:
                folder = pathlib.Path(name)
                budget = ExportBudget(folder, maximum, cancel or threading.Event(), progress, timeout)
                budget.archive = temporary
                snapshot_time = time.time_ns()
                progress(phase="snapshot", bytes_written=0)
                snapshot = self._snapshot(folder, budget)
                with contextlib.closing(sqlite3.connect(snapshot)) as db:
                    db.row_factory = sqlite3.Row
                    effective = dict(scope)
                    if scope.get("operation_id"):
                        operation = db.execute("SELECT * FROM operations WHERE operation_id=?", (scope["operation_id"],)).fetchone()
                        if operation is None:
                            raise KeyError("export operation not found")
                        if scope.get("device_id", operation["device_id"]) != operation["device_id"]:
                            raise ValueError("operation belongs to another device")
                        effective["device_id"] = operation["device_id"]
                        effective["since_ns"] = max(scope.get("since_ns", 0), operation["created_ns"])
                        effective["until_ns"] = min(scope.get("until_ns", 2**63 - 1), operation["finished_ns"] or time.time_ns())
                    effective.setdefault("until_ns", snapshot_time)
                    effective["until_ns"] = min(effective["until_ns"], snapshot_time)
                    budget.snapshot_bytes = sum(path.stat().st_size for path in folder.rglob("*") if path.is_file())
                    self._write_zip(temporary, folder, db, effective, budget)
                budget.check(disk=True)
            temporary.replace(target)
            return target
        finally:
            temporary.unlink(missing_ok=True)

    def _write_zip(self, target: pathlib.Path, folder: pathlib.Path, db: sqlite3.Connection,
                   scope: dict[str, Any], budget: ExportBudget) -> None:
        checksums: list[str] = []
        central_bytes = 0
        firmware_ids: set[str] = set()
        with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
            archive_file = archive.fp
            assert archive_file is not None
            def write(name: str, chunks: Iterable[bytes]) -> None:
                nonlocal central_bytes
                central_bytes += len(name.encode()) + 128
                budget.files += 1
                if budget.files > 4096:
                    raise OSError("export file budget exceeded")
                digest = hashlib.sha256()
                with archive.open(name, "w", force_zip64=True) as output:
                    for chunk in chunks:
                        budget.check(extra=len(chunk))
                        if budget.snapshot_bytes + archive_file.tell() + len(chunk) + central_bytes + CHUNK > budget.maximum:
                            raise OSError("export temporary disk budget exceeded; narrow the scope")
                        output.write(chunk)
                        digest.update(chunk)
                        budget.input_bytes += len(chunk)
                        if budget.input_bytes >= budget._next_disk_check:
                            budget.check(extra=CHUNK, disk=True)
                            budget._next_disk_check = budget.input_bytes + 1024 * 1024
                        budget.progress(phase="packing", bytes_written=target.stat().st_size,
                                        input_bytes=budget.input_bytes, files=budget.files)
                checksums.append(f"{digest.hexdigest()}  {name}\n")

            def records(sql: str, values=(), transform=lambda row: dict(row)):
                for row in db.execute(sql, values):
                    budget.check()
                    yield (encode_json(transform(row)) + "\n").encode()

            clauses, values = self._filters(scope, "created_ns")
            if scope.get("operation_id"):
                clauses, values = ["operation_id=?"], [scope["operation_id"]]
            db.execute("CREATE TEMP TABLE selected_operations AS SELECT * FROM operations WHERE " + " AND ".join(clauses), values)
            # Reject old oversized metadata before materializing Python objects.
            if db.execute("SELECT 1 FROM selected_operations WHERE length(CAST(params_json AS BLOB))+COALESCE(length(CAST(result_json AS BLOB)),0)+COALESCE(length(CAST(progress_json AS BLOB)),0)+COALESCE(length(error),0)>8388608 LIMIT 1").fetchone():
                raise OSError("operation metadata exceeds export record budget")
            def operation_row(row):
                item = StateStore._operation_row(row)
                artifact = item["params"].get("artifact_id")
                if artifact:
                    if len(firmware_ids) >= 4096:
                        raise OSError("export artifact budget exceeded")
                    firmware_ids.add(str(artifact))
                return item
            write("operations.jsonl", records("SELECT * FROM selected_operations ORDER BY created_ns", transform=operation_row))
            selected = "(SELECT operation_id FROM selected_operations)"
            for table, column in (("operation_reconciliations", "record_json"), ("operation_evidence", "payload_json")):
                if db.execute(f"SELECT 1 FROM {table} WHERE operation_id IN {selected} AND length(CAST({column} AS BLOB))>8388608 LIMIT 1").fetchone():
                    raise OSError("evidence metadata exceeds export record budget")
            write("reconciliations.jsonl", records("SELECT record_json FROM operation_reconciliations WHERE operation_id IN " + selected,
                                                   transform=lambda row: decode_json(row[0], {})))
            write("operation-evidence.jsonl", records("SELECT * FROM operation_evidence WHERE operation_id IN " + selected))
            # Project audit entries cannot safely be attributed to one device.
            audit_clauses, audit_values = self._filters(scope, "created_ns", device=False)
            if scope.get("device_id"):
                audit_clauses.append("0")
            if db.execute("SELECT 1 FROM system_audit WHERE " + " AND ".join(audit_clauses) +
                          " AND length(CAST(details_json AS BLOB))>8388608 LIMIT 1", audit_values).fetchone():
                raise OSError("audit metadata exceeds export record budget")
            def audit_row(row):
                item = dict(row)
                item["details"] = decode_json(item.pop("details_json"), {})
                return item
            write("system-audit.jsonl", records("SELECT * FROM system_audit WHERE " + " AND ".join(audit_clauses), audit_values, audit_row))
            write("history-gaps.jsonl", records("SELECT * FROM event_gaps ORDER BY start_id"))
            write("events.jsonl", self._event_records(folder, db, scope, budget))
            for path in self.artifacts_dir.rglob("*"):
                budget.check()
                if path.is_symlink():
                    raise OSError("export refuses symbolic links in artifacts")
                if not path.is_file():
                    continue
                relative = path.relative_to(self.artifacts_dir)
                if scope.get("device_id"):
                    safe_device = "".join(c for c in scope["device_id"] if c.isalnum() or c in "-_")
                    if relative.parts[0] == "firmware":
                        if len(relative.parts) < 3 or relative.parts[1] not in firmware_ids:
                            continue
                    elif relative.parts[0] != safe_device:
                        continue
                stamp = path.stat().st_mtime_ns
                if relative.parts[0] != "firmware" and not scope.get("since_ns", 0) <= stamp <= scope["until_ns"]:
                    continue
                with path.open("rb") as source:
                    original = os.fstat(source.fileno())
                    def file_chunks(original=original, source=source):
                        remaining = original.st_size
                        while remaining:
                            chunk = source.read(min(CHUNK, remaining))
                            if not chunk:
                                raise OSError("artifact changed during export")
                            remaining -= len(chunk)
                            yield chunk
                        current = os.fstat(source.fileno())
                        if (current.st_size, current.st_mtime_ns) != (original.st_size, original.st_mtime_ns):
                            raise OSError("artifact changed during export")
                    write("artifacts/" + relative.as_posix(), file_chunks())
            settings = {row[0]: decode_json(row[1], None) for row in db.execute(
                "SELECT key,value FROM settings WHERE key IN ('events.sequence','events.pruned_through','mode')")}
            manifest = {"schema": "esp-iris-export/v1", "format_revision": 2, "created_ns": time.time_ns(),
                        "scope": scope, "event_watermark": settings.get("events.sequence") or db.execute("SELECT COALESCE(MAX(event_id),0) FROM events").fetchone()[0],
                        "history_pruned_through": settings.get("events.pruned_through", 0),
                        "mode": settings.get("mode", "develop"), "history_format": "events.jsonl",
                        "audit_scope": "project" if not scope.get("device_id") else "omitted for device scope",
                        "legacy_raw_logs": "duplicate gzip history omitted; structured events and explicit gaps included"}
            write("manifest.json", [(encode_json(manifest) + "\n").encode()])
            write("checksums.sha256", ["".join(checksums).encode()])

    @staticmethod
    def _filters(scope: dict[str, Any], time_column: str, *, device: bool = True):
        clauses = [time_column + ">=?", time_column + "<=?"]
        values: list[Any] = [scope.get("since_ns", 0), scope.get("until_ns", 2**63 - 1)]
        if device and scope.get("device_id"):
            clauses.append("device_id=?")
            values.append(scope["device_id"])
        return clauses, values

    def _event_records(self, folder: pathlib.Path, state: sqlite3.Connection, scope: dict[str, Any], budget: ExportBudget):
        sources = [(folder / "state.sqlite3", None)] + [
            (folder / "segments" / self.events.segments.path(row[0]).name, row)
            for row in state.execute("SELECT start_id,last_id FROM event_segments WHERE status!='deleting' ORDER BY start_id")]
        for path, segment in sources:
            budget.check()
            if not path.exists():
                yield (encode_json({"kind": "history_gap", "reason": "missing segment at snapshot", "range": list(segment)}) + "\n").encode()
                continue
            with contextlib.closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as db:
                db.row_factory = sqlite3.Row
                db.execute("ATTACH DATABASE ? AS state", ((folder / "state.sqlite3").as_uri() + "?mode=ro",))
                clauses, values = self._filters(scope, "e.host_receive_ns", device=False)
                resolved = "COALESCE(e.device_id,(SELECT device_id FROM state.console_captures c WHERE c.capture_id=e.capture_id))"
                if scope.get("device_id"):
                    clauses.append(resolved + "=?")
                    values.append(scope["device_id"])
                sql = "SELECT e.event_id,e.category,e.host_receive_ns, " + resolved + " AS device_id, CASE WHEN length(CAST(e.payload_json AS BLOB))<=8388608 THEN e.payload_json ELSE NULL END AS payload_json FROM events e WHERE "
                for row in db.execute(sql + " AND ".join(clauses) + " ORDER BY e.event_id", values):
                    budget.check()
                    item = decode_json(row["payload_json"], {"kind": "history_gap", "reason": "event exceeds export record budget"})
                    item.update({key: row[key] for key in ("event_id", "category", "host_receive_ns", "device_id")})
                    yield (encode_json(item) + "\n").encode()
