from __future__ import annotations

import asyncio
import hashlib
import io
import json
import threading
import time
import zipfile

from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.export_jobs import ExportJobs, ExportPolicy
from iris_gateway.export_store import ExportStore
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.retention import RetentionPolicy
from iris_gateway.store import GatewayStore


def operation(store, key, device, created):
    store.create_operation({"operation_id": key, "device_id": device, "actor_type": "local", "actor_name": "test",
                            "action": "read", "params": {}, "status": "succeeded", "created_ns": created})


async def terminal(client, key):
    for _ in range(300):
        job = await (await client.get(f"/v2/exports/{key}")).json()
        if job["status"] not in {"queued", "running", "cancelling"}:
            return job
        await asyncio.sleep(.01)
    raise AssertionError("export did not finish")


def test_background_snapshot_survives_retention_and_streams_scoped_hashes(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        now = time.time_ns()
        operation(store, "op-a", "a", now - 100)
        operation(store, "op-b", "b", now - 100)
        store.append_event("log", {"kind": "log", "text": "a retained", "host_receive_wall_ns": now}, "a")
        store.append_event("log", {"kind": "log", "text": "b excluded", "host_receive_wall_ns": now}, "b")
        store.set_setting("password_hash", {"secret": "never-export"})
        started, release = threading.Event(), threading.Event()
        original = ExportStore._write_zip
        def paused(self, target, folder, db, scope, budget):
            started.set()
            while not release.wait(.005):
                budget.check()
            return original(self, target, folder, db, scope, budget)
        monkeypatch.setattr(ExportStore, "_write_zip", paused)
        service = GatewayService(store, instance_id="export")
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            response = await client.post("/v2/export", json={"device_id": "a"})
            assert response.status == 202
            key = (await response.json())["job_id"]
            while not started.is_set():
                await asyncio.sleep(.005)
            assert (await client.post("/v2/export", json={})).status == 429
            before = time.monotonic()
            assert (await client.get("/v2/health")).status == 200
            assert time.monotonic() - before < .25
            # Retire the original segment while its snapshot hard link remains.
            store.events.segments.prune(RetentionPolicy(event_rows=1))
            await service.on_device_event({"kind": "log", "device_id": "a", "text": "newer than snapshot"})
            await service.flush_events()
            release.set()
            job = await terminal(client, key)
            assert job["status"] == "completed", job
            download = await client.get(job["download_url"])
            data = await download.read()
            assert download.status == 200 and hashlib.sha256(data).hexdigest() == job["sha256"]
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                events = [json.loads(line) for line in archive.read("events.jsonl").splitlines()]
                assert [item["text"] for item in events] == ["a retained"]
                assert b"op-a" in archive.read("operations.jsonl") and b"op-b" not in archive.read("operations.jsonl")
                assert json.loads(archive.read("manifest.json"))["scope"]["device_id"] == "a"
                for line in archive.read("checksums.sha256").decode().splitlines():
                    digest, name = line.split("  ", 1)
                    assert hashlib.sha256(archive.read(name)).hexdigest() == digest
                assert all("never-export" not in archive.read(name).decode(errors="ignore") for name in archive.namelist())
            assert (await client.delete(f"/v2/exports/{key}")).status == 200
            assert not (tmp_path / "exports" / key).exists()
        finally:
            release.set()
            await client.close()
            store.close()
    asyncio.run(scenario())


def test_cancel_and_disk_budget_failures_reclaim_temporary_files(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="export")
        store.set_setting("exports.policy", {"job_bytes": 1024 * 1024, "total_bytes": 2 * 1024 * 1024})
        started = threading.Event()
        original = ExportStore._write_zip
        def blocked(self, target, folder, db, scope, budget):
            started.set()
            while True:
                budget.check()
                time.sleep(.005)
        monkeypatch.setattr(ExportStore, "_write_zip", blocked)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            key = (await (await client.post("/v2/export", json={})).json())["job_id"]
            while not started.is_set():
                await asyncio.sleep(.005)
            assert (await client.post(f"/v2/exports/{key}/cancel")).status == 200
            assert (await terminal(client, key))["status"] == "cancelled"
            assert [path.name for path in (tmp_path / "exports" / key).iterdir()] == ["status.json"]
            monkeypatch.setattr(ExportStore, "_write_zip", original)
            store.save_artifact("a", "large", b"x" * (2 * 1024 * 1024), "bin")
            key = (await (await client.post("/v2/export", json={})).json())["job_id"]
            job = await terminal(client, key)
            assert job["status"] == "failed" and "budget" in job["error"]
            assert [path.name for path in (tmp_path / "exports" / key).iterdir()] == ["status.json"]
        finally:
            await client.close()
            store.close()
    asyncio.run(scenario())


def test_restart_and_ttl_preserve_visible_failure_and_retained_archives(tmp_path):
    async def scenario():
        root = tmp_path / "exports"
        root.mkdir()
        old = time.time_ns() - 10_000_000_000
        for key, state, retained in [("a" * 32, "running", False), ("b" * 32, "completed", False), ("c" * 32, "completed", True)]:
            folder = root / key
            folder.mkdir()
            (folder / "status.json").write_text(json.dumps({"job_id": key, "status": state, "created_ns": old,
                                                           "finished_ns": old, "retain": retained}))
            (folder / "bundle.zip").write_bytes(b"zip")
            (folder / "snapshot-old").mkdir()
        manager = ExportJobs(tmp_path, ExportPolicy(ttl_seconds=1))
        try:
            assert manager.get("a" * 32)["status"] == "interrupted"
            assert not (root / ("a" * 32) / "bundle.zip").exists()
            assert not (root / ("b" * 32)).exists()
            assert manager.get("c" * 32)["status"] == "completed"
            assert not list(root.rglob("snapshot-*"))
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_operation_scope_keeps_evidence_and_limits_device_time_window(tmp_path):
    store = GatewayStore(tmp_path)
    try:
        operation(store, "selected", "a", 10)
        store.update_operation("selected", finished_ns=30)
        operation(store, "other", "b", 10)
        for device, stamp in [("a", 5), ("a", 10), ("a", 20), ("a", 30), ("a", 35), ("b", 20)]:
            store.append_event("log", {"kind": "log", "text": str(stamp), "host_receive_wall_ns": stamp}, device)
        store.db.execute("INSERT INTO operation_evidence VALUES(?,?,?,?,?)", ("selected", "healthy", "3", 31, '{"event_name":"healthy"}'))
        store.db.commit()
        archive = store.exports.export_zip(scope={"operation_id": "selected"})
        with zipfile.ZipFile(archive) as bundle:
            assert [json.loads(line)["text"] for line in bundle.read("events.jsonl").splitlines()] == ["10", "20", "30"]
            assert b"healthy" in bundle.read("operation-evidence.jsonl")
            assert b"other" not in bundle.read("operations.jsonl")
            assert bundle.read("system-audit.jsonl") == b""
    finally:
        store.close()


def test_interrupted_download_keeps_bundle_for_retry(tmp_path):
    import os

    async def scenario():
        store = GatewayStore(tmp_path)
        store.save_artifact("a", "download", os.urandom(8 * 1024 * 1024), "bin")
        service = GatewayService(store, instance_id="download")
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            key = (await (await client.post("/v2/export", json={})).json())["job_id"]
            job = await terminal(client, key)
            assert job["status"] == "completed", job
            response = await client.get(job["download_url"])
            await response.content.read(1024)
            response.close()
            for _ in range(100):
                job = await (await client.get(f"/v2/exports/{key}")).json()
                if job.get("last_download_error"):
                    break
                await asyncio.sleep(.01)
            assert "disconnected" in job["last_download_error"]
            retry = await client.get(job["download_url"])
            assert hashlib.sha256(await retry.read()).hexdigest() == job["sha256"]
            assert service.exports.get(key)["status"] == "completed"
        finally:
            await client.close()
            store.close()
    asyncio.run(scenario())


def test_export_storage_failure_does_not_prevent_gateway_health(tmp_path, monkeypatch):
    from pathlib import Path

    async def scenario():
        store = GatewayStore(tmp_path)
        original = Path.mkdir
        def fail_export_directory(path, *args, **kwargs):
            if path == tmp_path / "exports":
                raise OSError("export volume full")
            return original(path, *args, **kwargs)
        monkeypatch.setattr(Path, "mkdir", fail_export_directory)
        service = GatewayService(store, instance_id="disk-full")
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            health = await client.get("/v2/health")
            assert health.status == 200
            assert "volume full" in (await health.json())["exports"]["storage_error"]
            assert (await client.post("/v2/export", json={})).status == 503
            monkeypatch.setattr(Path, "mkdir", original)
            accepted = await client.post("/v2/export", json={})
            assert accepted.status == 202
            assert (await terminal(client, (await accepted.json())["job_id"]))["status"] == "completed"
        finally:
            await client.close()
            store.close()
    asyncio.run(scenario())
