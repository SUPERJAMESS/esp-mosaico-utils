from __future__ import annotations

import contextlib
import hashlib
import pathlib
import shutil
import sqlite3
import tempfile
import time
import zipfile

from .artifact_store import ArtifactStore
from .event_store import EventStore
from .state_store import StateStore
from .storage_json import encode_json as _json


class ExportStore:
    """Evidence export coordination across the independently retained stores."""

    def __init__(self, state: StateStore, events: EventStore, artifacts: ArtifactStore) -> None:
        self.state, self.root, self.db = state, state.root, state.db
        self.logs_dir, self.artifacts_dir = events.logs_dir, artifacts.artifacts_dir
        self.events = events

    def export_zip(self, target: pathlib.Path | None = None) -> pathlib.Path:
        if target is None:
            target = self.root / f"esp-iris-export-{time.strftime('%Y%m%d-%H%M%S')}.zip"
        manifest = {
            "schema": "esp-iris-export/v1",
            "created_ns": time.time_ns(),
            "mode": self.state.get_setting("mode", "develop"),
        }
        with tempfile.TemporaryDirectory(prefix="esp-iris-export-") as temp_name:
            temp = pathlib.Path(temp_name)
            (temp / "manifest.json").write_text(_json(manifest) + "\n", encoding="utf-8")
            with (temp / "operations.jsonl").open("w", encoding="utf-8") as handle:
                for item in reversed(self.state.operations(limit=1_000_000)):
                    handle.write(_json(item) + "\n")
            with (temp / "reconciliations.jsonl").open("w", encoding="utf-8") as handle:
                for row in self.db.execute("SELECT record_json FROM operation_reconciliations ORDER BY rowid"):
                    handle.write(row[0] + "\n")
            with (temp / "operation-evidence.jsonl").open("w", encoding="utf-8") as handle:
                for row in self.db.execute("SELECT * FROM operation_evidence ORDER BY operation_id, boot_id"):
                    handle.write(_json(dict(row)) + "\n")
            with (temp / "system-audit.jsonl").open("w", encoding="utf-8") as handle:
                for item in reversed(self.state.audits(limit=1_000_000)):
                    handle.write(_json(item) + "\n")
            # Include structured segments now that raw bodies are stored once.
            with self.events.segments.lock:
                event_folder = temp / "events"
                event_folder.mkdir()
                for row in self.db.execute("SELECT start_id FROM event_segments WHERE status!='deleting'"):
                    source = self.events.segments.path(row[0])
                    with contextlib.closing(sqlite3.connect(source)) as old, contextlib.closing(sqlite3.connect(event_folder / source.name)) as snapshot:
                        old.backup(snapshot)
            if self.logs_dir.exists():
                shutil.copytree(self.logs_dir, temp / "logs", dirs_exist_ok=True)
            if self.artifacts_dir.exists():
                shutil.copytree(self.artifacts_dir, temp / "artifacts", dirs_exist_ok=True)
            files = sorted(path for path in temp.rglob("*") if path.is_file())
            with (temp / "checksums.sha256").open("w", encoding="utf-8") as handle:
                for path in files:
                    digest = hashlib.sha256(path.read_bytes()).hexdigest()
                    handle.write(f"{digest}  {path.relative_to(temp).as_posix()}\n")
            with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(p for p in temp.rglob("*") if p.is_file()):
                    archive.write(path, path.relative_to(temp).as_posix())
        return target


