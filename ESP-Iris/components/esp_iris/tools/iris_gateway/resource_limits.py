"""Explicit admission and byte budgets for shared host resources."""
from __future__ import annotations

import asyncio
import dataclasses
import time
from collections import deque
from typing import Any

from .storage_json import encode_json


class ResourceExhausted(RuntimeError):
    def __init__(self, resource: str, limit: int) -> None:
        super().__init__(f"{resource} capacity is {limit}; retry after active work completes")
        self.resource, self.limit = resource, limit


@dataclasses.dataclass(frozen=True)
class GatewayLimits:
    subscribers: int = 32
    subscribers_per_actor: int = 8
    subscriber_items: int = 1024
    subscriber_bytes: int = 1024 * 1024
    event_bytes: int = 256 * 1024
    writer_items: int = 1024
    writer_bytes: int = 4 * 1024 * 1024
    operations: int = 64
    operations_per_device: int = 8
    operations_per_actor: int = 32
    operation_bytes: int = 128 * 1024 * 1024
    requests: int = 32
    large_requests: int = 2
    json_bytes: int = 1024 * 1024
    upload_bytes: int = 128 * 1024 * 1024
    background_jobs: int = 4
    observed_jobs: int = 512
    auth_sessions: int = 128
    media_streams: int = 8
    media_subscribers: int = 16
    media_bytes: int = 16 * 1024 * 1024 + 4096
    hub_observers: int = 64
    history_devices: int = 64
    device_history_bytes: int = 1024 * 1024
    endpoints: int = 64

    def __post_init__(self) -> None:
        for field in dataclasses.fields(self):
            value = getattr(self, field.name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                raise ValueError(f"{field.name} must be a positive integer")
        if self.subscriber_items < 2 or self.subscriber_bytes < self.event_bytes + 512:
            raise ValueError("subscriber budget must fit an event and a history-gap marker")
        if min(self.media_bytes, self.device_history_bytes) < 1024:
            raise ValueError("history and media byte budgets must be at least 1024")


def event_size(item: dict[str, Any]) -> int:
    data = item.get("data")
    if isinstance(data, bytes):
        return len(data) + len(encode_json({key: value for key, value in item.items() if key != "data"}).encode())
    return len(encode_json(item).encode())


class ByteBudgetQueue(asyncio.Queue):
    def __init__(self, maxsize: int, max_bytes: int) -> None:
        super().__init__(maxsize=maxsize)
        self.max_bytes, self.bytes = max_bytes, 0
        self._sizes: Any = deque()
        self._next_size = 0

    def __iter__(self):
        return iter(self._queue)

    def append(self, item: dict[str, Any]) -> None:
        self.push(item)

    def put_nowait(self, item: dict[str, Any], *, size: int | None = None) -> None:
        amount = event_size(item) if size is None else size
        if self.bytes + amount > self.max_bytes:
            raise asyncio.QueueFull
        self._next_size = amount
        super().put_nowait(item)

    def _put(self, item: dict[str, Any]) -> None:
        self._sizes.append(self._next_size)
        self.bytes += self._next_size
        super()._put(item)

    def _get(self) -> dict[str, Any]:
        self.bytes -= self._sizes.popleft()
        return super()._get()

    def push(self, item: dict[str, Any], size: int | None = None) -> int:
        amount = event_size(item) if size is None else size
        if amount > self.max_bytes - 512:
            item = {"kind": "history_gap", "reason": "event exceeds subscriber byte budget", "dropped_events": 1}
            amount = event_size(item)
        if self.qsize() < self.maxsize and self.bytes + amount <= self.max_bytes:
            self.put_nowait(item, size=amount)
            return 0
        dropped = 0
        while not self.empty() and (self.qsize() > self.maxsize - 2 or self.bytes + amount + 512 > self.max_bytes):
            dropped += int(self.get_nowait().get("dropped_events", 1))
        gap = {"kind": "history_gap", "category": "system", "host_receive_ns": time.time_ns(),
               "reason": "live subscriber queue overflow", "dropped_events": dropped}
        self.put_nowait(gap)
        self.put_nowait(item, size=amount)
        return dropped


class OperationAdmission:
    def __init__(self, limits: GatewayLimits) -> None:
        self.limits = limits
        self.reservations: dict[str, tuple[object, str, str, int]] = {}

    def acquire(self, operation_id: str, device: str, actor: str, cost: int,
                token: object | None = None) -> object:
        existing = self.reservations.get(operation_id)
        if existing is not None:
            if token is existing[0]:
                return token
            raise ResourceExhausted("operation registration", 1)
        rows = list(self.reservations.values())
        checks = [("operations", len(rows), self.limits.operations),
                  ("operations per device", sum(row[1] == device for row in rows), self.limits.operations_per_device),
                  ("operations per actor", sum(row[2] == actor for row in rows), self.limits.operations_per_actor)]
        for name, used, maximum in checks:
            if used >= maximum:
                raise ResourceExhausted(name, maximum)
        if cost < 0 or sum(row[3] for row in rows) + cost > self.limits.operation_bytes:
            raise ResourceExhausted("operation payload bytes", self.limits.operation_bytes)
        token = object()
        self.reservations[operation_id] = (token, device, actor, cost)
        return token

    def release(self, operation_id: str, token: object) -> None:
        if operation_id in self.reservations and self.reservations[operation_id][0] is token:
            del self.reservations[operation_id]
