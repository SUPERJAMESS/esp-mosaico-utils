"""Public, standard-library-only local host API, version 2.

Consumers select a state root and project identity; storage layout, locks and
schema compatibility belong to Iris. Readers never launch a service, migrate a
database or retain a client lease. Registry/project probes create no state;
terminal evidence reads acquire an Iris lock to serialize with a new owner.
"""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import sqlite3
import time
from typing import Any

from .link import EndpointLock
from .source_identity import source_identity

__all__ = ["API_MAJOR", "LocalProject", "LocalStateError", "inspect_system_update_bundle", "registry_snapshot", "source_identity"]

API_MAJOR = 2
REGISTRY_VERSION = 200


def inspect_system_update_bundle(source: Any) -> dict[str, Any]:
    """Validate a local path or raw .irisfw bytes without starting a Gateway.

    Returns owned metadata, including validated component descriptors. Loading
    optional bundle dependencies is deferred until this explicit operation.
    Bundle format/cryptographic checks stay owned by Iris; product partition
    policies remain the caller's responsibility. Raises ValueError on failure.
    """
    from .system_update import load_system_update_bundle

    bundle = load_system_update_bundle(source)
    return {"chip_id": bundle.chip_id, "flash_size": bundle.flash_size,
            "components": [component.as_dict() for component in bundle.components]}


class LocalStateError(RuntimeError):
    """Unavailable or incompatible local Iris state; never recreate it blindly."""


def registry_snapshot(state_root: pathlib.Path) -> dict[str, Any]:
    """Read same-user sessions and claims, including confirmed lock liveness."""
    root = state_root.resolve() / "ownership"
    database = root / "ownership.sqlite3"
    if not database.is_file():
        return {"schema_version": API_MAJOR, "sessions": [], "claims": []}
    try:
        with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True, timeout=2)) as db:
            db.row_factory = sqlite3.Row
            with db:
                db.execute("BEGIN")
                version = db.execute("PRAGMA user_version").fetchone()[0]
                if version != REGISTRY_VERSION:
                    raise LocalStateError(f"Unsupported Iris ownership schema {version}; expected {REGISTRY_VERSION}")
                sessions = [dict(row) for row in db.execute(
                    "SELECT session_id,project_id,project_path,instance_id,url,created_ns,persistent FROM sessions")]
                claims = [dict(row) for row in db.execute(
                    "SELECT resource,owner,generation,state,device_id,metadata,transfer_id FROM claims")]
                metadata = {row["session_id"]: dict(row) for row in db.execute(
                    "SELECT session_id,workspace_path,lifecycle_capability,source_revision FROM session_metadata"
                )}
        for session in sessions:
            session.update(metadata.get(session["session_id"], {}))
            session["alive"] = EndpointLock.held("session:" + session["session_id"], root / "locks")
        alive = {item["session_id"]: item["alive"] for item in sessions}
        for claim in claims:
            claim["metadata"] = json.loads(claim["metadata"])
            claim["owner_alive"] = alive.get(claim["owner"], False)
        return {"schema_version": API_MAJOR, "sessions": sessions, "claims": claims}
    except (OSError, sqlite3.Error, ValueError) as error:
        raise LocalStateError(f"Could not read the shared Iris registry: {error}") from error


class LocalProject:
    """Stable project locator and launch coordination for an Iris host client."""

    def __init__(self, state_root: pathlib.Path, workspace: pathlib.Path, project: pathlib.Path,
                 *, data_dir: pathlib.Path | None = None) -> None:
        self.root = state_root.resolve()
        self.workspace, self.project = workspace.resolve(), project.resolve()
        self.project_id = hashlib.sha256((str(self.workspace) + "\0" + str(self.project)).encode()).hexdigest()
        self.directory = self.root / "project-sessions" / self.project_id
        self.connection_file = self.directory / "connection.json"
        self._data_override = data_dir
        self.data_directory = self.project / ".gateway"
        self.location_file = self.directory / "data-location.json"
        if data_dir is not None:
            self.data_directory = data_dir.expanduser().resolve()
        elif self.location_file.exists():
            saved = json.loads(self.location_file.read_text(encoding="utf-8"))
            if saved.get("project_id") != self.project_id:
                raise LocalStateError("Gateway data locator belongs to another project")
            self.data_directory = pathlib.Path(saved["data_directory"]).expanduser().resolve()
        self.state_directory = self.data_directory / "state"
        legacy = self.directory / "state"
        self.credentials_directory = legacy if (legacy / "gateway.sqlite3").exists() else self.directory / "credentials"
        self.log_file = self.data_directory / "gateway.log"

    def prepare_storage(self) -> None:
        """Prepare project diagnostics without copying host identities or locks."""
        lock = EndpointLock("gateway-state:" + os.path.normcase(str(self.state_directory)))
        try:
            lock.acquire()
            self._prepare_storage()
        finally:
            lock.close()

    def _prepare_storage(self) -> None:
        legacy = self.directory / "state"
        if (legacy / "gateway.sqlite3").exists() and not (self.state_directory / "gateway.sqlite3").exists():
            raise LocalStateError(
                "Existing Gateway history needs an offline move. Stop its Gateway, then run "
                f"esp_iris.py storage --state-dir {str(legacy)!r} --migrate-to {str(self.state_directory)!r}. "
                "Back up the source before upgrading; it remains the live host credentials store.")
        self.data_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        marker = self.data_directory / "project.json"
        if (self.data_directory != self.project / ".gateway" and marker.exists()
                and json.loads(marker.read_text(encoding="utf-8")).get("project_id") != self.project_id):
            raise LocalStateError("Gateway data directory is already assigned to another project")
        marker.write_text(json.dumps({"project_id": self.project_id, "project": str(self.project)}) + "\n", encoding="utf-8")
        ignore = self.data_directory / ".gitignore"
        if not ignore.exists():
            ignore.write_text("*\n", encoding="utf-8")
        if self._data_override is not None:
            self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
            temporary = self.location_file.with_suffix(".tmp")
            temporary.write_text(json.dumps({"project_id": self.project_id,
                                             "data_directory": str(self.data_directory)}), encoding="utf-8")
            temporary.replace(self.location_file)

    @contextlib.contextmanager
    def starting(self):
        """Serialize launch/admission. Mutating API; queries must not call it."""
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        lock = EndpointLock("project-start:" + self.project_id, root=self.directory / "locks")
        try:
            lock.acquire(blocking=True)
            yield
        finally:
            lock.close()

    def running(self) -> bool:
        return EndpointLock.held("project:" + self.project_id, self.root / "ownership" / "locks")

    def connection(self) -> dict[str, Any]:
        return json.loads(self.connection_file.read_text(encoding="utf-8"))

    def discard_stopped_connection(self) -> None:
        """Caller must hold starting(); live owners are never replaced."""
        if self.running():
            raise LocalStateError("This project's Gateway is alive but unavailable; inspect its log before retrying")
        with contextlib.suppress(FileNotFoundError):
            self.connection_file.unlink()

    def launch_arguments(self, session_id: str) -> list[str]:
        """Arguments for the pinned esp_iris.py web entrypoint."""
        return ["--state-dir", str(self.state_directory),
                "--credentials-dir", str(self.credentials_directory),
                "--process-log", str(self.log_file),
                "--project-session-id", session_id, "--project-id", self.project_id,
                "--project-path", str(self.project), "--project-connection-file", str(self.connection_file),
                "--ownership-dir", str(self.root / "ownership"),
                "--project-shared", "--project-workspace", str(self.workspace)]

    def finished_operation(self, record: dict[str, Any], operation_id: str) -> dict[str, Any] | None:
        """Read terminal evidence from this launch after it has released its lock.

        Returns None while the owner is live or evidence is unavailable. Unknown
        storage versions fail closed and are never migrated by a reader.
        """
        from .schema import SCHEMA_VERSION
        from .store import GatewayStore

        if record.get("project_id") != self.project_id or not record.get("created_ns"):
            return None
        if not self.state_directory.exists():
            return None
        lock = EndpointLock("project:" + self.project_id, root=self.root / "ownership" / "locks")
        deadline = time.monotonic() + 2
        try:
            while True:
                try:
                    lock.acquire()
                    break
                except RuntimeError:
                    if time.monotonic() >= deadline:
                        return None
                    time.sleep(0.05)
            database = self.state_directory / "gateway.sqlite3"
            with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as db:
                db.row_factory = sqlite3.Row
                if db.execute("PRAGMA user_version").fetchone()[0] != SCHEMA_VERSION:
                    return None
                row = db.execute("SELECT * FROM operations WHERE operation_id=?", (operation_id,)).fetchone()
            if row is None or row["created_ns"] < record["created_ns"] or not row["finished_ns"]:
                return None
            if row["status"] not in {"succeeded", "failed", "cancelled", "interrupted", "outcome_unknown"}:
                return None
            return GatewayStore._operation_row(row)
        except (OSError, sqlite3.Error, ValueError, KeyError):
            return None
        finally:
            lock.close()
