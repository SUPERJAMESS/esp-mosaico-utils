from __future__ import annotations

import asyncio
import base64
import sqlite3

import pytest

from iris_gateway.cli import _recover_interrupted
from iris_gateway.event_store import EventStore
from iris_gateway.gateway import GatewayService
from iris_gateway.security import Actor
from iris_gateway.storage_health import StorageUnavailable
from iris_gateway.store import GatewayStore


@pytest.mark.parametrize("failure", [sqlite3.OperationalError("database full"),
                                     OSError(28, "No space left"), PermissionError("read-only logs")])
def test_failed_event_storage_keeps_live_delivery_and_recovers(tmp_path, monkeypatch, failure):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="fault")
        queue = service.subscribe()
        append = EventStore.append_batch
        attempts = 0

        def broken(*args, **kwargs):
            nonlocal attempts
            attempts += 1
            raise failure

        monkeypatch.setattr(EventStore, "append_batch", broken)
        for index in range(10):
            await service.on_device_event({"kind": "log", "text": str(index)})
            assert (await asyncio.wait_for(queue.get(), 2))["kind"] == "history_gap"
            item = await asyncio.wait_for(queue.get(), 2)
            assert item["text"] == str(index) and item["persisted"] is False
        assert attempts == 1
        assert service._event_writer.snapshot()["dropped_events"] == 10
        assert service._event_writer.snapshot()["status"] == "degraded"
        monkeypatch.setattr(EventStore, "append_batch", append)
        await asyncio.sleep(1.05)
        await service.on_device_event({"kind": "log", "text": "recovered"})
        gap = await asyncio.wait_for(queue.get(), 2)
        assert gap["kind"] == "history_gap" and gap["dropped_events"] == 10
        assert (await asyncio.wait_for(queue.get(), 2))["text"] == "recovered"
        assert service._event_writer.snapshot()["status"] == "ok"
        assert store.latest_events()[0]["dropped_events"] == 10
        store.close()

    asyncio.run(scenario())


def test_capture_recovery_records_known_missing_bytes(tmp_path):
    store = GatewayStore(tmp_path)
    event = {"kind": "console_raw", "capture_id": "a" * 32, "endpoint": "usb:test",
             "offset": 0, "data_base64": base64.b64encode(b"test").decode()}
    store.record_event("console", event)
    store.db.execute("PRAGMA query_only=ON")
    assert store.record_event("console", {**event, "offset": 4})[-1]["persisted"] is False
    store.db.execute("PRAGMA query_only=OFF")
    store._retry_events_at = 0
    store.record_event("console", {**event, "offset": 8})
    history = store.latest_events(capture_id=event["capture_id"])
    assert [item["kind"] for item in history] == ["console_raw", "console_gap", "console_raw"]
    assert history[1]["offset"] == 4 and history[1]["end_offset"] == 8
    assert history[-1]["offset"] == 8
    store.close()


@pytest.mark.parametrize("background", [False, True])
def test_unwritable_state_rejects_device_call_before_admission(tmp_path, background):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="admission")
        called = False

        async def command():
            nonlocal called
            called = True

        store.db.execute("PRAGMA query_only=ON")
        method = service.operations.submit if background else service.operations.execute
        with pytest.raises(StorageUnavailable):
            await method("device", Actor("local", "test"), "firmware.ota", {}, command)
        assert not called
        assert not store.operations()
        store.db.execute("PRAGMA query_only=OFF")
        result, _, _ = await service.operations.execute(
            "device", Actor("local", "test"), "firmware.ota", {}, command)
        assert called and result["status"] == "succeeded"
        await service.operations.close()
        store.close()

    asyncio.run(scenario())


def test_failed_result_commit_is_unknown_after_restart(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="result")

        async def command():
            store.db.execute("PRAGMA query_only=ON")
            return {"accepted": True}

        with pytest.raises(StorageUnavailable):
            await service.operations.execute("device", Actor("local", "test"),
                                             "firmware.ota", {}, command, operation_id="write")
        assert store.operation("write")["status"] == "running"
        store.close()
        recovered = GatewayStore(tmp_path)
        _recover_interrupted(recovered)
        assert recovered.operation("write")["status"] == "outcome_unknown"
        recovered.close()

    asyncio.run(scenario())
