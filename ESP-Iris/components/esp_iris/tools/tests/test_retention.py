from __future__ import annotations

import dataclasses
import json
import os
import subprocess
import sys
import time

import pytest

from iris_gateway.cli import main
from iris_gateway.process_log import RotatingLog
from iris_gateway.retention import RetentionPolicy, maintain, prune_events
from iris_gateway.store import GatewayStore


def test_retention_is_bounded_preserves_evidence_and_persists_gap(tmp_path):
    store = GatewayStore(tmp_path)
    store.set_setting("sentinel", "protected")
    store.create_operation({"operation_id": "ota", "device_id": "device", "actor_type": "local",
                            "actor_name": "test", "action": "firmware.ota", "status": "outcome_unknown",
                            "created_ns": 1})
    # A healthy observation from a pre-upgrade 0.2 store, not yet bound to an operation.
    store.db.execute("INSERT INTO events(device_id,category,host_receive_ns,payload_json) VALUES(?,?,?,?)",
                     ("device", "device", 2, json.dumps({"event_name": "healthy", "boot_id": 17})))
    store.db.commit()
    for index in range(9):
        store.db.execute("INSERT INTO events(category,host_receive_ns,payload_json) VALUES('device',?,?)",
                         (3 + index, json.dumps({"kind": "log"})))
    store.db.commit()
    policy = RetentionPolicy(event_rows=2, batch_rows=3)
    assert prune_events(store, policy) == 3
    assert len(store.latest_events()) == 7
    assert store.healthy_evidence("ota", 17)["boot_id"] == 17
    assert prune_events(store, policy) == 3
    assert prune_events(store, policy) == 3
    assert prune_events(store, policy) == 1
    assert prune_events(store, policy) == 0
    store.close()
    store = GatewayStore(tmp_path)
    assert store.events_after(1) == ([], True)
    assert store.operation("ota")["status"] == "outcome_unknown"
    assert store.get_setting("sentinel") == "protected"
    assert store.append_event("device", {"kind": "new"})["event_id"] == 11
    store.close()


@pytest.mark.parametrize("limit", ["event_rows", "event_bytes"])
def test_each_capacity_budget_prunes_recent_events(tmp_path, limit):
    store = GatewayStore(tmp_path)
    for _ in range(20):
        store.db.execute("INSERT INTO events(category,host_receive_ns,payload_json) VALUES('device',?,?)",
                         (time.time_ns(), json.dumps({"data": "x" * 4096})))
    store.db.commit()
    policy = RetentionPolicy(**{limit: 0}, batch_rows=5)
    assert prune_events(store, policy) == 5
    assert len(store.latest_events()) == 15
    store.close()


def test_background_maintenance_uses_saved_policy(tmp_path):
    store = GatewayStore(tmp_path)
    store.set_setting("storage.retention", dataclasses.asdict(RetentionPolicy(event_rows=2)))
    for _ in range(10):
        store.append_event("device", {"kind": "log"})
    result = maintain(tmp_path)
    assert result["removed_events"] == 10
    assert len(store.latest_events()) <= 2
    store.close()


def test_offline_preview_does_not_delete_and_compaction_is_explicit(tmp_path, capsys):
    store = GatewayStore(tmp_path)
    store.append_event("device", {"host_receive_wall_ns": 1})
    store.close()
    args = ["storage", "--state-dir", str(tmp_path)]
    assert main(args) == 0
    preview = json.loads(capsys.readouterr().out)
    assert preview["event_rows"] == 1 and preview["applied"] is False
    assert main([*args, "--compact"]) == 2
    capsys.readouterr()
    assert main([*args, "--apply", "--compact"]) == 0
    assert json.loads(capsys.readouterr().out)["removed_events"] == 1


def test_process_logs_rotate_long_lines_and_expire(tmp_path):
    path = tmp_path / "gateway.log"
    path.write_bytes(b"old")
    os.utime(path, (time.time() - 9 * 86400,) * 2)
    log = RotatingLog(path, max_bytes=100, backups=2)
    assert not path.exists()
    log.write(b"x" * 1000 + b"latest")
    files = list(tmp_path.iterdir())
    assert len(files) == 3
    assert all(item.stat().st_size <= 100 for item in files)
    assert path.read_bytes().endswith(b"latest")


def test_process_log_captures_native_child_output(tmp_path):
    code = """
import os, pathlib, subprocess, sys
from iris_gateway.process_log import redirect_process_log
with redirect_process_log(pathlib.Path(sys.argv[1]), max_bytes=100):
    os.write(1, b'parent\\n')
    subprocess.run([sys.executable, '-c', 'import os; os.write(2, b"x" * 1000 + b"child-end")'], check=True)
"""
    path = tmp_path / "gateway.log"
    subprocess.run([sys.executable, "-c", code, str(path)], check=True, timeout=10)
    assert path.read_bytes().endswith(b"child-end")
    assert all(item.stat().st_size <= 100 for item in tmp_path.iterdir())
