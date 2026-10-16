from __future__ import annotations

import asyncio
import base64
import os
import subprocess
import sys
import threading
from types import SimpleNamespace

from iris_gateway.event_segments import EventSegments
from iris_gateway.event_writer import EventWriter
from iris_gateway.gateway import GatewayService
from iris_gateway.resource_limits import GatewayLimits
from iris_gateway.store import GatewayStore


def test_slow_disk_does_not_block_loop_and_batches_coalesce_devices(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="batch")
        service.attach_hub(SimpleNamespace(list_devices=lambda: [{"device_id": "device", "session_id": 1}]))
        entered, release = threading.Event(), threading.Event()
        append = EventSegments.append

        def delayed(self, rows):
            entered.set()
            assert release.wait(timeout=5)
            return append(self, rows)

        monkeypatch.setattr(EventSegments, "append", delayed)
        try:
            await service.on_device_event({"kind": "log", "device_id": "device", "text": "first"})
            while not entered.is_set():
                await asyncio.sleep(0.005)
            # These events remain bounded and the producer completes while disk
            # I/O is deliberately blocked on another thread.
            for index in range(500):
                await service.on_device_event({"kind": "log", "device_id": "device", "text": str(index)})
            assert service._event_writer.snapshot()["queued_events"] == 500
            ticks = 0
            for _ in range(5):
                await asyncio.sleep(0.005)
                ticks += 1
            assert ticks == 5 and not release.is_set()
            release.set()
            await service.flush_events()
            snapshot = service._event_writer.snapshot()
            assert snapshot["written_events"] == 501
            assert snapshot["batches"] <= 6
            assert snapshot["device_writes"] == 1
            assert len(store.latest_events()) == 501
        finally:
            release.set()
            await service.close_events()
            store.close()

    asyncio.run(scenario())


def test_full_writer_queue_reports_loss_and_critical_evidence_waits_for_commit(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        store.create_operation({"operation_id": "ota", "device_id": "device", "actor_type": "local",
                                "actor_name": "test", "action": "firmware.ota", "status": "outcome_unknown",
                                "created_ns": 1})
        service = GatewayService(store, instance_id="overflow", limits=GatewayLimits(writer_items=8, writer_bytes=16 * 1024))
        queue = service.subscribe()
        entered, release = threading.Event(), threading.Event()
        append = EventSegments.append

        def delayed(self, rows):
            entered.set()
            assert release.wait(timeout=5)
            return append(self, rows)

        monkeypatch.setattr(EventSegments, "append", delayed)
        monkeypatch.setattr(EventWriter, "MAX_ITEMS", 8)
        monkeypatch.setattr(EventWriter, "MAX_BYTES", 16 * 1024)
        raw = {"kind": "console_raw", "capture_id": "a" * 32, "endpoint": "uart:test",
               "data_base64": base64.b64encode(b"data").decode()}
        try:
            await service.on_device_event({**raw, "offset": 0})
            while not entered.is_set():
                await asyncio.sleep(0.005)
            for index in range(20):
                await service.on_device_event({**raw, "offset": (index + 1) * 4})
            snapshot = service._event_writer.snapshot()
            assert snapshot["queued_events"] == 8 and snapshot["queued_bytes"] <= 16 * 1024
            assert snapshot["dropped_events"] == 12
            assert any(item["kind"] == "history_gap" for item in list(queue._queue))
            healthy = asyncio.create_task(service.on_device_event({"kind": "device_event", "device_id": "device",
                                                                  "event_name": "healthy", "boot_id": 7}))
            await asyncio.sleep(0)
            assert not healthy.done() and store.healthy_evidence("ota", 7) is None
            release.set()
            await healthy
            await service.on_device_event({**raw, "offset": 84})
            await service.flush_events()
            assert store.healthy_evidence("ota", 7)["boot_id"] == 7
            history = store.latest_events()
            assert sum(row.get("dropped_events", 0) for row in history if row["kind"] == "history_gap") == 12
            gap = next(row for row in history if row["kind"] == "console_gap")
            assert gap["offset"] == 36 and gap["end_offset"] == 84
            assert history[-1]["offset"] == 84
        finally:
            release.set()
            await service.close_events()
            store.close()

    asyncio.run(scenario())


def test_abrupt_exit_reports_uncertain_buffer_window_on_restart(tmp_path):
    script = """
import asyncio, os, pathlib, threading, sys
from iris_gateway.event_segments import EventSegments
from iris_gateway.gateway import GatewayService
from iris_gateway.store import GatewayStore
entered = threading.Event()
def blocked(self, rows):
    entered.set()
    threading.Event().wait(10)
EventSegments.append = blocked
async def run():
    service = GatewayService(GatewayStore(pathlib.Path(sys.argv[1])), instance_id='crash')
    await service.on_device_event({'kind': 'log', 'text': 'buffered'})
    while not entered.is_set():
        await asyncio.sleep(0.005)
    os._exit(17)
asyncio.run(run())
"""
    result = subprocess.run([sys.executable, "-c", script, str(tmp_path)], env=os.environ.copy(), timeout=10, check=False)
    assert result.returncode == 17

    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="restarted")
        queue = service.subscribe()
        await service.on_device_event({"kind": "log", "text": "after restart"})
        first = await asyncio.wait_for(queue.get(), 3)
        assert first["kind"] == "history_gap" and "previous writer" in first["reason"]
        assert (await asyncio.wait_for(queue.get(), 3))["text"] == "after restart"
        await service.close_events()
        assert store.get_setting("events.writer_active") is False
        store.close()

    asyncio.run(scenario())
