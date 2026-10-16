"""One bounded diagnostics writer; SQLite connections stay on their owner thread."""
from __future__ import annotations

import asyncio
import concurrent.futures
import logging
import pathlib
import sqlite3
import threading
import time
from collections import OrderedDict, deque
from collections.abc import Callable
from typing import Any

from .storage_health import StorageUnavailable
from .storage_json import encode_json
from .store import GatewayStore

logger = logging.getLogger(__name__)


class EventWriter:
    MAX_ITEMS = 1024
    MAX_BYTES = 4 * 1024 * 1024
    BATCH_ITEMS = 128
    BATCH_BYTES = 512 * 1024

    def __init__(self, root: pathlib.Path, publish: Callable[[list[dict[str, Any]]], None], *,
                 max_items: int | None = None, max_bytes: int | None = None,
                 max_event_bytes: int = 256 * 1024) -> None:
        self.root, self.publish = root, publish
        self.loop = asyncio.get_running_loop()
        self.max_items = self.MAX_ITEMS if max_items is None else max_items
        self.max_bytes = self.MAX_BYTES if max_bytes is None else max_bytes
        self.max_event_bytes = max_event_bytes
        self.condition = threading.Condition()
        self.items: Any = deque()
        self.bytes = 0
        self.inflight = False
        self.closing = False
        self.error: str | None = None
        self.lost = 0
        self.pending_loss = 0
        self.loss_start = 0
        self.loss_end = 0
        self.lost_captures: OrderedDict[str, tuple[int, int]] = OrderedDict()
        self.batches = 0
        self.written = 0
        self.max_commit_seconds = 0.0
        self.device_writes = 0
        self.device_cache: OrderedDict[str, tuple[str, float]] = OrderedDict()
        self.thread = threading.Thread(target=self._run, name="iris-event-writer", daemon=True)
        self.thread.start()

    def submit(self, category: str, item: dict[str, Any], device: dict[str, Any] | None,
               *, critical: bool = False) -> tuple[bool, Any]:
        size = len(encode_json(item).encode()) + (len(encode_json(device).encode()) if device else 0)
        future: concurrent.futures.Future[bool] | None = concurrent.futures.Future() if critical else None
        with self.condition:
            # Reserve a small amount of space for evidence acknowledgements.
            extra_items, extra_bytes = (16, self.BATCH_BYTES) if critical else (0, 0)
            if (self.closing or size > self.max_event_bytes or len(self.items) >= self.max_items + extra_items
                    or self.bytes + size > self.max_bytes + extra_bytes):
                self._note_loss(item)
                return False, None
            self.items.append((size, category, item, device, future))
            self.bytes += size
            self.condition.notify()
        return True, future

    def _note_loss(self, item: dict[str, Any]) -> None:
        stamp = int(item.get("host_receive_wall_ns") or time.time_ns())
        if not self.pending_loss:
            self.loss_start = stamp
        self.loss_end = stamp
        self.pending_loss += 1
        self.lost += 1
        capture = item.get("capture_id")
        if capture and item.get("kind") == "console_raw":
            start = int(item["offset"])
            old = self.lost_captures.get(capture)
            encoded = item["data_base64"]
            raw_size = len(encoded) // 4 * 3 - (len(encoded) - len(encoded.rstrip("=")))
            self.lost_captures[capture] = (old[0] if old else start, start + raw_size)
            self.lost_captures.move_to_end(capture)
            while len(self.lost_captures) > 1024:
                self.lost_captures.popitem(last=False)

    def _take(self, store: GatewayStore) -> list[Any]:
        with self.condition:
            while not self.items and not self.closing:
                self.condition.wait(timeout=0.05)
            if not self.items:
                return []
            batch: list[Any] = []
            size = 0
            while self.items and len(batch) < self.BATCH_ITEMS:
                item = self.items[0]
                special = item[2].get("kind") == "console_binding" or item[4] is not None
                if batch and (special or size + item[0] > self.BATCH_BYTES):
                    break
                self.items.popleft()
                self.bytes -= item[0]
                size += item[0]
                batch.append(item)
                if special:
                    break
            self.inflight = True
            self._merge_loss(store)
            return batch

    def _merge_loss(self, store: GatewayStore) -> None:
        if self.pending_loss:
            if not store._pending_loss:
                store._loss_start_ns = self.loss_start
            store._loss_end_ns = self.loss_end
            store._pending_loss += self.pending_loss
            store._lost_events += self.pending_loss
            store._lost_captures.update(self.lost_captures)
            self.pending_loss = 0
            self.lost_captures.clear()

    def _devices(self, batch: list[Any]) -> list[dict[str, Any]]:
        now = time.monotonic()
        devices = {str(item[3]["device_id"]): item[3] for item in batch if item[3] is not None}
        changed = []
        for device_id, info in devices.items():
            encoded = encode_json(info)
            previous = self.device_cache.get(device_id)
            if previous is None or previous[0] != encoded or now - previous[1] >= 5:
                changed.append(info)
                self.device_cache[device_id] = (encoded, now)
                self.device_cache.move_to_end(device_id)
        cache_bytes = sum(len(value[0].encode()) for value in self.device_cache.values())
        while len(self.device_cache) > 1024 or cache_bytes > self.max_bytes:
            cache_bytes -= len(next(iter(self.device_cache.values()))[0].encode())
            self.device_cache.popitem(last=False)
        return changed

    def _deliver(self, result: list[dict[str, Any]]) -> None:
        if not self.loop.is_closed():
            self.loop.call_soon_threadsafe(self.publish, result)

    def _run(self) -> None:
        store = None
        batch: list[Any] = []
        try:
            store = GatewayStore(self.root)
            store.db.execute("PRAGMA busy_timeout=100")
            interrupted = store.get_setting("events.writer_active", False)
            store.set_setting("events.writer_active", True)
            if interrupted:
                self._deliver(store.record_batch([("system", {"kind": "history_gap",
                    "reason": "previous writer exited before flushing; buffered events may be missing"}, None)], []))
            while True:
                batch = self._take(store)
                if not batch:
                    break
                devices = self._devices(batch)
                started = time.monotonic()
                result = store.record_batch([(category, item, item.get("device_id"))
                                             for _, category, item, _, _ in batch], devices)
                self.max_commit_seconds = max(self.max_commit_seconds, time.monotonic() - started)
                self.batches += 1
                self.written += sum("event_id" in item for item in result)
                self.error = store.storage_health()["event_error"]
                if self.error:
                    self.device_cache.clear()
                else:
                    self.device_writes += len(devices)
                self._deliver(result)
                for _, _, _, _, future in batch:
                    if future is not None and not future.done():
                        future.set_result(not self.error)
                with self.condition:
                    self.lost = max(self.lost, store._lost_events + self.pending_loss)
                    self.inflight = False
                batch = []
        except Exception as exc:
            logger.exception("event writer stopped")
            # Surface worker failures and release all acknowledgements. Never
            # leave a device callback waiting for an exited writer thread.
            with self.condition:
                self.error = str(exc)
                self.closing = True
                stranded = [*batch, *self.items]
                self.items.clear()
                self.bytes = 0
            for _, category, item, _, future in stranded:
                with self.condition:
                    self._note_loss(item)
                self._deliver([dict(item, category=category, persisted=False),
                               {"kind": "history_gap", "reason": "event writer stopped", "dropped_events": 1}])
                if future is not None and not future.done():
                    future.set_result(False)
            with self.condition:
                self.inflight = False
        finally:
            if store is not None:
                try:
                    with self.condition:
                        self._merge_loss(store)
                    if store._pending_loss:
                        self._deliver(store.record_batch([], []))
                    if not store._pending_loss:
                        store.set_setting("events.writer_active", False)
                except (sqlite3.Error, OSError, StorageUnavailable) as exc:
                    self.error = str(exc)
                finally:
                    store.close()

    def snapshot(self) -> dict[str, Any]:
        with self.condition:
            return {"status": "degraded" if self.error else "ok", "error": self.error,
                    "queued_events": len(self.items), "queued_bytes": self.bytes,
                    "dropped_events": self.lost, "written_events": self.written,
                    "batches": self.batches, "max_commit_seconds": self.max_commit_seconds,
                    "device_writes": self.device_writes}

    async def flush(self) -> None:
        while True:
            with self.condition:
                if not self.items and not self.inflight:
                    return
            await asyncio.sleep(0.005)

    def close_sync(self) -> None:
        with self.condition:
            self.closing = True
            self.condition.notify_all()
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            raise RuntimeError("event writer did not stop within its shutdown budget")

    async def close(self) -> None:
        await self.loop.run_in_executor(None, self.close_sync)
