from __future__ import annotations

import json
import os
import shutil
import sqlite3

import pytest

from iris_gateway.client import LocalProject, LocalStateError
from iris_gateway.gateway import GatewayService
from iris_gateway.link import EndpointLock
from iris_gateway.security import Actor, AuthManager
from iris_gateway.state_store import StateStore
from iris_gateway.storage_migration import migrate_project_data
from iris_gateway.store import GatewayStore


def test_project_data_moves_without_host_runtime_or_credentials(tmp_path):
    host = tmp_path / "host"
    workspace = tmp_path / "workspace"
    app = workspace / "app"
    project = LocalProject(host, workspace, app)
    project.prepare_storage()
    store = GatewayStore(project.state_directory)
    credentials = StateStore(project.credentials_directory)
    service = GatewayService(store, instance_id="project", credentials_store=credentials)
    service.auth.set_initial_password("private-password")
    store.append_event("device", {"message": "project history"})
    assert store.get_setting("password_hash") is None
    assert store.get_setting("host_id") is None
    assert (app / ".gateway/.gitignore").read_text() == "*\n"
    credentials.close()
    store.close()
    moved = tmp_path / "moved"
    shutil.copytree(app, moved)
    copied = LocalProject(host, workspace, moved)
    copied.prepare_storage()
    assert copied.project_id != project.project_id
    assert copied.connection_file != project.connection_file
    assert copied.credentials_directory != project.credentials_directory
    copy_store = GatewayStore(copied.state_directory)
    assert copy_store.latest_events()[0]["message"] == "project history"
    assert copy_store.get_setting("password_hash") is None
    copy_store.close()


def test_local_override_is_remembered_and_cannot_be_shared_by_projects(tmp_path):
    project = LocalProject(tmp_path / "host", tmp_path, tmp_path / "readonly",
                           data_dir=tmp_path / "disk")
    project.prepare_storage()
    assert not (tmp_path / "readonly").exists()
    reopened = LocalProject(tmp_path / "host", tmp_path, tmp_path / "readonly")
    assert reopened.state_directory == tmp_path / "disk/state"
    assert "--credentials-dir" in reopened.launch_arguments("session")
    other = LocalProject(tmp_path / "host", tmp_path, tmp_path / "other", data_dir=tmp_path / "disk")
    with pytest.raises(LocalStateError, match="another project"):
        other.prepare_storage()


def test_old_history_requires_explicit_offline_migration_and_keeps_credentials(tmp_path):
    project = LocalProject(tmp_path / "host", tmp_path, tmp_path / "app")
    legacy = project.directory / "state"
    store = GatewayStore(legacy)
    auth = AuthManager(store)
    auth.set_initial_password("private-password")
    auth.create_agent_token("agent", Actor("local", "test"))
    store.set_setting("host_id", "old-host")
    store.append_event("device", {"message": "old history"})
    artifact = store.save_artifact("device", "crash", b"saved evidence", "bin")
    relative_artifact = artifact.relative_to(legacy)
    store.close()
    original = (legacy / "gateway.sqlite3").read_bytes()
    with pytest.raises(LocalStateError, match="offline move"):
        project.prepare_storage()
    report = migrate_project_data(legacy, project.state_directory)
    assert report["source_retained"] and not report["credentials_copied"]
    assert (legacy / "gateway.sqlite3").read_bytes() == original
    reopened = LocalProject(tmp_path / "host", tmp_path, tmp_path / "app")
    reopened.prepare_storage()
    assert reopened.credentials_directory == legacy
    migrated = GatewayStore(project.state_directory)
    assert migrated.latest_events()[0]["message"] == "old history"
    assert migrated.get_setting("password_hash") is None
    assert not migrated.db.execute("SELECT * FROM agent_tokens").fetchall()
    assert (project.state_directory / relative_artifact).read_bytes() == b"saved evidence"
    migrated.close()
    with pytest.raises(FileExistsError):
        migrate_project_data(legacy, project.state_directory)


def test_migration_refuses_running_source_and_retries_interrupted_copy(tmp_path):
    source, target = tmp_path / "old", tmp_path / "new"
    store = GatewayStore(source)
    store.close()
    lock = EndpointLock("gateway-state:" + os.path.normcase(str(source.resolve())))
    lock.acquire()
    try:
        with pytest.raises(RuntimeError):
            migrate_project_data(source, target)
    finally:
        lock.close()
    stage = tmp_path / ".new.importing"
    stage.mkdir()
    (stage / "migration-source.json").write_text(json.dumps({"source": str(source), "target": str(target)}))
    (stage / "gateway.sqlite3").write_bytes(b"interrupted copy")
    migrate_project_data(source, target)
    assert not stage.exists()
    with sqlite3.connect(target / "gateway.sqlite3") as db:
        assert db.execute("PRAGMA quick_check").fetchone()[0] == "ok"


def test_migration_failure_leaves_source_and_no_published_target(tmp_path, monkeypatch):
    source, target = tmp_path / "old", tmp_path / "new"
    store = GatewayStore(source)
    store.save_artifact("device", "crash", b"evidence", "bin")
    store.close()

    def fail(*args, **kwargs):
        raise OSError("disk full")

    monkeypatch.setattr(shutil, "copytree", fail)
    with pytest.raises(OSError, match="disk full"):
        migrate_project_data(source, target)
    assert not target.exists() and not (tmp_path / ".new.importing").exists()
    assert (source / "gateway.sqlite3").exists()
