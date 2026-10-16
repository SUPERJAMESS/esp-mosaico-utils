from __future__ import annotations

import base64
import dataclasses
import pathlib
import sqlite3

import pytest

from iris_gateway.retention import RetentionPolicy
from iris_gateway.store import GatewayStore


def configure(store, **kwargs):
    policy = RetentionPolicy(segment_bytes=16384, **kwargs)
    store.set_setting("storage.retention", dataclasses.asdict(policy))
    return policy


def test_segment_rotation_queries_and_retirement_keep_global_cursors(tmp_path):
    store = GatewayStore(tmp_path)
    policy = configure(store, segment_files=3)
    for index in range(12):
        store.append_event("log" if index % 2 else "device", {"index": index, "text": "x" * 8192},
                           "a" if index % 3 else "b")
    assert store.db.execute("SELECT COUNT(*) FROM events").fetchone()[0] == 0
    assert len(list((tmp_path / "events").glob("*.sqlite3"))) > 3
    history, gap = store.events_after(2, device_id="a", categories=["log"])
    assert not gap
    assert [item["index"] for item in history] == [5, 7, 11]
    assert [item["index"] for item in store.latest_events(limit=3)] == [9, 10, 11]
    while store.events.segments.prune(policy):
        pass
    files = list((tmp_path / "events").glob("*.sqlite3"))
    assert len(files) <= 3
    remaining, gap = store.events_after(1)
    assert gap and remaining[-1]["index"] == 11
    store.close()
    reopened = GatewayStore(tmp_path)
    assert reopened.append_event("device", {"index": 12})["event_id"] == 13
    assert reopened.events_after(1)[1]
    reopened.close()


def test_identity_binding_resolves_across_closed_segments_without_inventing_boot(tmp_path):
    store = GatewayStore(tmp_path)
    configure(store)
    capture = "a" * 32
    raw = {"kind": "console_raw", "capture_id": capture, "endpoint": "uart:test", "boot_id": None,
           "offset": 0, "data_base64": base64.b64encode(b"ROM" * 4096).decode()}
    first = store.append_event("console", raw)
    store.append_event("device", {"kind": "connection", "device_id": "device"}, "device")
    binding = store.append_event("console", {"kind": "console_binding", "capture_id": capture,
                                             "offset": 12288, "boot_id": 42}, "device")
    assert binding["history_start_event_id"] == first["event_id"]
    history = store.latest_events(device_id="device", endpoint="uart:test", capture_id=capture)
    assert history[0]["boot_id"] is None and history[0]["device_id"] == "device"
    assert base64.b64decode(history[0]["data_base64"]) == b"ROM" * 4096
    store.close()
    reopened = GatewayStore(tmp_path)
    assert reopened.latest_events(device_id="device", capture_id=capture)[0]["boot_id"] is None
    reopened.close()


def test_crash_between_catalog_and_event_commit_preserves_evidence_and_gap(tmp_path, monkeypatch):
    store = GatewayStore(tmp_path)
    store.create_operation({"operation_id": "ota", "device_id": "device", "actor_type": "local",
                            "actor_name": "test", "action": "firmware.ota", "status": "outcome_unknown",
                            "created_ns": 1})
    connect = sqlite3.connect

    class FailedCommit:
        def __init__(self, db):
            self.db = db

        def __getattr__(self, name):
            return getattr(self.db, name)

        def commit(self):
            raise sqlite3.OperationalError("injected segment commit failure")

    def failing(path, *args, **kwargs):
        db = connect(path, *args, **kwargs)
        return FailedCommit(db) if pathlib.Path(str(path)).name.startswith("segment-") else db

    with monkeypatch.context() as patch:
        patch.setattr(sqlite3, "connect", failing)
        result = store.record_event("device", {"event_name": "healthy", "boot_id": 7}, "device")
        assert result[-1]["persisted"] is False
    store.close()
    recovered = GatewayStore(tmp_path)
    assert recovered.healthy_evidence("ota", 7)["boot_id"] == 7
    assert recovered.append_event("device", {"kind": "recovered"})["event_id"] == 2
    rows, gap = recovered.events_after(0)
    assert gap and [row["event_id"] for row in rows] == [2]
    recovered.close()


def test_interrupted_retirement_is_hidden_then_finished_on_retry(tmp_path, monkeypatch):
    store = GatewayStore(tmp_path)
    store.append_event("log", {"text": "old"})
    policy = RetentionPolicy(event_rows=0)
    unlink = pathlib.Path.unlink

    def failing(path, *args, **kwargs):
        if path.name.startswith("segment-"):
            raise OSError("injected unlink failure")
        return unlink(path, *args, **kwargs)

    with monkeypatch.context() as patch:
        patch.setattr(pathlib.Path, "unlink", failing)
        with pytest.raises(OSError, match="unlink failure"):
            store.events.segments.prune(policy)
    assert store.latest_events() == []
    store.close()
    reopened = GatewayStore(tmp_path)
    assert reopened.events.segments.prune(policy) == 1
    assert not list((tmp_path / "events").glob("*.sqlite3"))
    assert reopened.append_event("log", {"text": "new"})["event_id"] == 2
    reopened.close()


def test_missing_active_segment_is_reported_and_never_silently_replaced(tmp_path):
    store = GatewayStore(tmp_path)
    store.append_event("log", {"text": "old"})
    next((tmp_path / "events").glob("*.sqlite3")).unlink()
    assert store.events_after(0)[1]
    assert store.record_event("log", {"text": "new"})[-1]["persisted"] is False
    assert not list((tmp_path / "events").glob("*.sqlite3"))
    store.close()
