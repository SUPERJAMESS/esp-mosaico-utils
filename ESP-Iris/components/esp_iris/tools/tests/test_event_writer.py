from __future__ import annotations

import asyncio
import base64
import os
import sqlite3
import subprocess
import sys
import threading
from types import SimpleNamespace

from iris_gateway.event_segments import EventSegments
from iris_gateway.event_writer import EventWriter
from iris_gateway.gateway import GatewayService
from iris_gateway.resource_limits import GatewayLimits
from iris_gateway.store import GatewayStore


def test_queued_inventory_is_shared_within_the_byte_budget(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="shared-snapshot",
                                 limits=GatewayLimits(writer_items=16, writer_bytes=8192))
        info = {"device_id": "device", "metadata": "x" * 4096, "app_version": "old"}
        service.attach_hub(SimpleNamespace(list_devices=lambda: [dict(info)]))
        entered, release = threading.Event(), threading.Event()
        append = EventSegments.append

        def delayed(self, rows):
            entered.set()
            assert release.wait(5)
            return append(self, rows)

        monkeypatch.setattr(EventSegments, "append", delayed)
        try:
            await service.on_device_event({"kind": "log", "text": "warmup"})
            while not entered.is_set():
                await asyncio.sleep(0.005)
            for index in range(8):
                info["app_version"] = str(index)
                await service.on_device_event({"kind": "log", "device_id": "device", "text": str(index)})
            snapshot = service._event_writer.snapshot()
            assert snapshot["queued_events"] == 8 and snapshot["queued_bytes"] <= 8192
            assert snapshot["dropped_events"] == 0
            assert len(service._event_writer.pending_devices) == 1
            release.set()
            await service.flush_events()
            assert service._event_writer.snapshot()["queued_bytes"] == 0
            assert not service._event_writer.pending_devices
            assert store.cached_devices()[0]["app_version"] == "7"
            assert len(store.latest_events()) == 9
        finally:
            release.set()
            await service.close_events()
            store.close()

    asyncio.run(scenario())


def test_failed_older_batch_cannot_shrink_already_reported_console_gap(tmp_path):
    store = GatewayStore(tmp_path)
    capture = "b" * 32
    raw = {"kind": "console_raw", "capture_id": capture, "endpoint": "uart:test",
           "data_base64": base64.b64encode(b"data").decode()}
    try:
        store.record_event("console", {**raw, "offset": 0})
        # A full producer queue reports a later gap before the earlier batch
        # fails to commit on the worker. Both loss windows must survive.
        store._lost_captures[capture] = (12, 16)
        store._retry_events_at = float("inf")
        store.record_event("console", {**raw, "offset": 4})
        store.record_event("console", {**raw, "offset": 8})
        assert store._lost_captures[capture] == (4, 16)
        store._retry_events_at = 0
        result = store.record_event("console", {**raw, "offset": 16})
        gap = next(item for item in store.latest_events() if item["kind"] == "console_gap")
        assert (gap["offset"], gap["end_offset"]) == (4, 16)
        assert result[-1]["offset"] == 16 and "event_id" in result[-1]
    finally:
        store.close()


def test_queued_console_bytes_between_lost_ranges_are_preserved(tmp_path):
    store = GatewayStore(tmp_path)
    capture = "c" * 32
    raw = {"kind": "console_raw", "capture_id": capture, "endpoint": "uart:test",
           "data_base64": base64.b64encode(b"data").decode()}
    try:
        store.append_event("console", {**raw, "offset": 0})
        # While a batch waits, a full producer queue drops [4,8) and
        # [12,16), but accepts [8,12). The loss bound includes that queued row.
        store._lost_captures[capture] = (4, 16)
        store.append_event("console", {**raw, "offset": 8})
        store.append_event("console", {**raw, "offset": 16})
        rows = store.latest_events()
        assert [(item["offset"], item["end_offset"]) for item in rows if item["kind"] == "console_gap"] == [(4, 8), (12, 16)]
        assert [item["offset"] for item in rows if item["kind"] == "console_raw"] == [0, 8, 16]
    finally:
        store.close()


def test_sqlite_contention_preserves_batch_and_listings_do_not_write(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="contention")
        devices = [{"device_id": f"device-{index}", "session_id": index} for index in range(9)]
        service.attach_hub(SimpleNamespace(list_devices=lambda: devices, active_mirrors=lambda _: [], list_endpoints=list))
        # Initialize the worker before holding an independent state writer.
        await service.on_device_event({"kind": "log", "text": "ready"})
        await service.flush_events()
        blocker = sqlite3.connect(tmp_path / "gateway.sqlite3")
        blocker.execute("BEGIN IMMEDIATE")
        try:
            writes = store.db.total_changes
            for _ in range(9):
                assert len(service.list_devices()) == 9
            assert store.db.total_changes == writes
            for index in range(9):
                await service.on_device_event({"kind": "log", "device_id": f"device-{index}", "text": str(index)})
            # Longer than the diagnostics connection's 100 ms busy timeout.
            # The loop must continue servicing requests while the batch waits.
            for _ in range(30):
                await asyncio.sleep(0.01)
            assert service._event_writer.snapshot()["dropped_events"] == 0
            blocker.rollback()
            await asyncio.wait_for(service.flush_events(), 5)
            snapshot = service._event_writer.snapshot()
            assert snapshot["status"] == "ok" and snapshot["dropped_events"] == 0
            assert snapshot["written_events"] == 10
            assert len(store.cached_devices()) == 9
            assert not any(row["kind"] == "history_gap" for row in store.latest_events())
        finally:
            blocker.close()
            await service.close_events()
            store.close()

    asyncio.run(scenario())


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


def test_interleaved_console_overflow_recovers_and_preserves_healthy(tmp_path, monkeypatch):
    async def scenario():
        store = GatewayStore(tmp_path)
        store.create_operation({"operation_id": "update", "device_id": "device", "actor_type": "local",
                                "actor_name": "test", "action": "firmware.system_update",
                                "status": "outcome_unknown", "created_ns": 1})
        service = GatewayService(store, instance_id="interleaved",
                                 limits=GatewayLimits(writer_items=2, writer_bytes=16 * 1024))
        entered = [threading.Event(), threading.Event()]
        release = [threading.Event(), threading.Event()]
        append = EventSegments.append
        calls = 0
        def delayed(self, rows):
            nonlocal calls
            index = calls
            calls += 1
            if index < 2:
                entered[index].set()
                assert release[index].wait(timeout=5)
            return append(self, rows)
        monkeypatch.setattr(EventSegments, "append", delayed)
        raw = {"kind": "console_raw", "capture_id": "b" * 32, "endpoint": "uart:test",
               "data_base64": base64.b64encode(b"data").decode()}
        async def send(offset):
            await service.on_device_event({**raw, "offset": offset})
        async def wait_entered(index):
            for _ in range(1000):
                if entered[index].is_set():
                    return
                await asyncio.sleep(0.001)
            raise AssertionError("writer did not enter the expected batch")
        try:
            await send(0)
            await wait_entered(0)
            for offset in [4, 8, 12]:
                await send(offset)  # 12 is dropped while 4 and 8 are queued.
            release[0].set()
            await wait_entered(1)
            for offset in [16, 20, 24]:
                await send(offset)  # Accepted bytes separate two lost ranges.
            release[1].set()
            await service.flush_events()
            assert service._event_writer.snapshot()["status"] == "ok"
            await send(28)
            await service.on_device_event({"kind": "device_event", "device_id": "device",
                                           "event_name": "healthy", "boot_id": 7})
            await service.flush_events()
            history = store.latest_events()
            assert [e["offset"] for e in history if e["kind"] == "console_raw"] == [0, 4, 8, 16, 20, 28]
            assert [(e["offset"], e["end_offset"]) for e in history if e["kind"] == "console_gap"] == [(12, 16), (24, 28)]
            assert store.healthy_evidence("update", 7)["boot_id"] == 7
            assert service._event_writer.snapshot()["dropped_events"] == 2
        finally:
            for gate in release:
                gate.set()
            await service.close_events()
            store.close()
    asyncio.run(scenario())
