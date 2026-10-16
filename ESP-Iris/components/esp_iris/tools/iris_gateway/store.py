from __future__ import annotations

import base64
import pathlib
import sqlite3
import time
from collections.abc import Callable, Iterable
from typing import Any

from .artifact_store import ArtifactStore
from .event_store import EventStore
from .export_store import ExportStore
from .state_store import StateStore


class GatewayStore(StateStore):
    """Public facade: durable state plus disposable history and retained artifacts."""

    def __init__(self, root: pathlib.Path) -> None:
        super().__init__(root)
        self._closers: list[Callable[[], None]] = []
        try:
            self.events = EventStore(self, root)
            self.artifacts = ArtifactStore(self, root)
            self.exports = ExportStore(self, self.events, self.artifacts)
        except BaseException:
            self.close()
            raise
        self.logs_dir = self.events.logs_dir
        self.artifacts_dir = self.artifacts.artifacts_dir
        self._lost_captures = self.events._lost_captures
        self._event_error: str | None = self.events.recovery_error
        self._retry_events_at = 0.0
        self._lost_events = 0
        self._pending_loss = 0
        self._loss_start_ns = 0
        self._loss_end_ns = 0

    def before_close(self, callback: Callable[[], None]) -> None:
        self._closers.append(callback)

    def close(self) -> None:
        for callback in getattr(self, "_closers", []):
            callback()
        self._closers = []
        super().close()

    def record_batch(self, events: list[tuple[str, dict[str, Any], str | None]],
                     devices: list[dict[str, Any]]) -> list[dict[str, Any]]:
        now = time.monotonic()
        if now >= self._retry_events_at:
            try:
                prefix = []
                if self._pending_loss:
                    prefix.append(("system", {"kind": "history_gap", "reason": "event storage unavailable",
                                               "dropped_events": self._pending_loss,
                                               "from_host_receive_ns": self._loss_start_ns,
                                               "to_host_receive_ns": self._loss_end_ns}, None))
                result = self.events.append_batch([*prefix, *events], devices)
                self._pending_loss = 0
                self._event_error = self._state_error = None
                self._retry_events_at = 0
                return result
            except (sqlite3.Error, OSError) as exc:
                self.storage_failed(exc)
                self._event_error = str(exc)
                self._retry_events_at = now + 1
        result = []
        for category, payload, device_id in events:
            result.extend(self.record_event(category, payload, device_id))
        return result

    def storage_health(self) -> dict[str, Any]:
        return {"status": "degraded" if self._event_error or self._state_error else "ok",
                "state_error": self._state_error, "event_error": self._event_error,
                "lost_events": self._lost_events, "pending_loss": self._pending_loss}

    def record_event(self, category: str, payload: dict[str, Any],
                     device_id: str | None = None,
                     device: dict[str, Any] | None = None) -> list[dict[str, Any]]:
        """Best-effort diagnostics never interrupt the device receive loop."""
        now = time.monotonic()
        if now >= self._retry_events_at:
            try:
                if device is not None:
                    self.remember_device(device)
                recovered = []
                if self._pending_loss:
                    recovered.append(self.append_event("system", {
                        "kind": "history_gap", "reason": "event storage unavailable",
                        "dropped_events": self._pending_loss,
                        "from_host_receive_ns": self._loss_start_ns,
                        "to_host_receive_ns": self._loss_end_ns,
                    }))
                    self._pending_loss = 0
                item = self.append_event(category, payload, device_id)
                self._event_error = None
                self._state_error = None
                self._retry_events_at = 0
                return [*recovered, item]
            except (sqlite3.Error, OSError) as exc:
                self.storage_failed(exc)
                self._event_error = str(exc)
                self._retry_events_at = now + 1.0
        host_ns = int(payload.get("host_receive_wall_ns") or time.time_ns())
        self._lost_events += 1
        if not self._pending_loss:
            self._loss_start_ns = host_ns
        self._loss_end_ns = host_ns
        self._pending_loss += 1
        capture = payload.get("capture_id")
        if capture and payload.get("kind") == "console_raw":
            offset = int(payload["offset"])
            previous = self._lost_captures.get(str(capture))
            self._lost_captures[str(capture)] = (previous[0] if previous else offset, offset + len(
                base64.b64decode(payload["data_base64"], validate=True)))
            self._lost_captures.move_to_end(str(capture))
            while len(self._lost_captures) > 1024:
                self._lost_captures.popitem(last=False)
        item = dict(payload, category=category, host_receive_ns=host_ns, persisted=False)
        gap = {"kind": "history_gap", "category": "system", "host_receive_ns": host_ns,
               "reason": "event storage unavailable", "dropped_events": 1}
        return [gap, item]

    def find_healthy_evidence(self, operation: dict[str, Any], boot: Any) -> dict[str, Any] | None:
        healthy = self.healthy_evidence(operation["operation_id"], boot)
        if healthy is None:
            # Existing 0.2 history is preserved lazily, never scanned at startup.
            events = self.latest_events(device_id=operation["device_id"], categories=["device"], limit=5000)
            healthy = next((event for event in reversed(events)
                            if event.get("event_name") == "healthy" and event.get("boot_id") == boot
                            and event.get("host_receive_ns", 0) >= operation["created_ns"]), None)
            if healthy:
                with self.db:
                    self.preserve_event_evidence(healthy, operation["device_id"])
        return healthy

    def append_event(self, category: str, payload: dict[str, Any], device_id: str | None = None) -> dict[str, Any]:
        return self.events.append_event(category, payload, device_id)

    def events_after(self, cursor: int, *, device_id: str | None = None,
                     categories: Iterable[str] | None = None, capture_id: str | None = None,
                     endpoint: str | None = None, limit: int = 1000) -> tuple[list[dict[str, Any]], bool]:
        return self.events.events_after(cursor, device_id=device_id, categories=categories,
                                       capture_id=capture_id, endpoint=endpoint, limit=limit)

    def latest_events(self, *, device_id: str | None = None, categories: Iterable[str] | None = None,
                      capture_id: str | None = None, endpoint: str | None = None,
                      limit: int = 1000) -> list[dict[str, Any]]:
        return self.events.latest_events(device_id=device_id, categories=categories,
                                        capture_id=capture_id, endpoint=endpoint, limit=limit)

    def cleanup_raw_logs(self, max_age_days: int = 7, max_bytes: int = 1024 * 1024 * 1024) -> None:
        self.events.cleanup_raw_logs(max_age_days, max_bytes)

    def save_firmware_artifact(self, *, binary: bytes, elf: bytes, map_data: bytes,
                               metadata: dict[str, Any]) -> dict[str, Any]:
        return self.artifacts.save_firmware_artifact(binary=binary, elf=elf, map_data=map_data, metadata=metadata)

    def firmware_artifact(self, artifact_id: str) -> dict[str, Any] | None:
        return self.artifacts.firmware_artifact(artifact_id)

    def firmware_artifacts(self, limit: int = 200) -> list[dict[str, Any]]:
        return self.artifacts.firmware_artifacts(limit)

    def save_artifact(self, device_id: str, kind: str, data: bytes, suffix: str) -> pathlib.Path:
        return self.artifacts.save_artifact(device_id, kind, data, suffix)

    def export_zip(self, target: pathlib.Path | None = None) -> pathlib.Path:
        return self.exports.export_zip(target)


__all__ = ["GatewayStore"]
