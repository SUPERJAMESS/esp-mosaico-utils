"""Exercise upgrades against a frozen pre-change schema, including rollback copies."""
from __future__ import annotations

import hashlib
import json
import shutil
import sqlite3
from contextlib import closing
from pathlib import Path

import pytest

from iris_gateway.schema import SCHEMA_VERSION
from iris_gateway.state_store import StateStore
from iris_gateway.storage_migration import migrate_project_data
from iris_gateway.store import GatewayStore


def legacy_state(root, version):
    root.mkdir()
    with closing(sqlite3.connect(root / "gateway.sqlite3")) as db:
        db.executescript((Path(__file__).parent / "fixtures/gateway-schema-200.sql").read_text())
        db.executemany("INSERT INTO settings VALUES(?,?,1)", [
            ("password_hash", json.dumps("private-password-hash")),
            ("host_id", json.dumps("original-host")), ("tls", json.dumps("private-tls-path"))])
        db.execute("INSERT INTO agent_tokens(token_id,name,token_hash,created_ns) VALUES('token','agent','private-token-hash',1)")
        db.execute("INSERT INTO operations(operation_id,device_id,actor_type,actor_name,action,params_json,status,created_ns) "
                   "VALUES('op','device','local','test','firmware.ota','{}','outcome_unknown',1)")
        db.execute("INSERT INTO console_captures VALUES('capture','usb:old','device',1,2,4,4)")
        db.execute("INSERT INTO events(device_id,category,host_receive_ns,payload_json,capture_id) VALUES(NULL,'console',2,?,'capture')",
                   (json.dumps({"kind": "log", "text": "existing capture"}),))
        if version == 201:
            db.executescript("CREATE TABLE operation_evidence (operation_id TEXT NOT NULL REFERENCES operations(operation_id), "
                             "kind TEXT NOT NULL, boot_id TEXT NOT NULL, host_receive_ns INTEGER NOT NULL, payload_json TEXT NOT NULL, "
                             "PRIMARY KEY(operation_id,kind,boot_id)); PRAGMA user_version=201;")
            db.execute("INSERT INTO operation_evidence VALUES('op','healthy','0000000000000007',3,?)",
                       (json.dumps({"event_name": "healthy", "boot_id": 7}),))
        db.commit()
    (root / "logs").mkdir()
    (root / "logs/history.jsonl.gz").write_bytes(b"retained legacy log bytes")
    (root / "artifacts").mkdir()
    (root / "artifacts/evidence.bin").write_bytes(b"retained crash evidence")
    (root / "gateway.key").write_text("private-key")


def snapshot(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


@pytest.mark.parametrize("version", [200, 201])
@pytest.mark.parametrize("relocate", [False, True])
def test_legacy_upgrade_preserves_history_and_rollback_backup(tmp_path, version, relocate):
    source, backup = tmp_path / "old", tmp_path / "backup"
    legacy_state(source, version)
    before = snapshot(source)
    shutil.copytree(source, backup)
    target = tmp_path / "project/state" if relocate else source
    if relocate:
        migrate_project_data(source, target)
        assert snapshot(source) == before
    store = GatewayStore(target)
    try:
        assert store.schema_version == SCHEMA_VERSION
        event = store.latest_events()[0]
        assert event["text"] == "existing capture" and event["device_id"] == "device"
        captured, gap = store.events_after(0, capture_id="capture")
        assert captured == [event] and not gap
        assert store.operation("op")["status"] == "outcome_unknown"
        if version == 201:
            assert store.db.execute("SELECT payload_json FROM operation_evidence").fetchone()[0] == json.dumps(
                {"event_name": "healthy", "boot_id": 7})
        assert store.append_event("log", {"text": "new history"})["event_id"] > event["event_id"]
        assert store.db.execute("PRAGMA quick_check").fetchone()[0] == "ok"
        for name in ("logs/history.jsonl.gz", "artifacts/evidence.bin"):
            assert hashlib.sha256((target / name).read_bytes()).hexdigest() == before[name]
        if relocate:
            assert all(store.get_setting(key) is None for key in ("password_hash", "host_id", "tls"))
            assert not store.db.execute("SELECT * FROM agent_tokens").fetchall()
            assert not (target / "gateway.key").exists()
            assert b"private-token-hash" not in (target / "gateway.sqlite3").read_bytes()
    finally:
        store.close()
    if relocate:
        # The retained source is a live credentials store, not an immutable backup.
        credentials = StateStore(source)
        assert credentials.schema_version == SCHEMA_VERSION
        assert credentials.get_setting("host_id") == "original-host"
        credentials.close()
    assert snapshot(backup) == before
    restored = tmp_path / "restored"
    shutil.copytree(backup, restored)
    with closing(sqlite3.connect(restored / "gateway.sqlite3")) as old:
        assert old.execute("PRAGMA user_version").fetchone()[0] == version
        assert old.execute("SELECT count(*) FROM events").fetchone()[0] == 1
        assert old.execute("SELECT token_hash FROM agent_tokens").fetchone()[0] == "private-token-hash"
