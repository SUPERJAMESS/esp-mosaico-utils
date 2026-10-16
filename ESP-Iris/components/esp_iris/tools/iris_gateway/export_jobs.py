"""Bounded background export lifecycle and retained/downloadable bundles."""
from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import hashlib
import json
import pathlib
import shutil
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from typing import Any

from aiohttp import web

from .export_store import CHUNK, ExportCancelled, export_scope
from .resource_limits import ResourceExhausted
from .storage_health import StorageUnavailable
from .store import GatewayStore


@dataclasses.dataclass(frozen=True)
class ExportPolicy:
    job_bytes: int = 1024**3
    total_bytes: int = 2 * 1024**3
    ttl_seconds: int = 3600
    timeout_seconds: int = 300
    jobs: int = 32

    def __post_init__(self):
        if any(type(getattr(self, item.name)) is not int or getattr(self, item.name) <= 0 for item in dataclasses.fields(self)):
            raise ValueError("export limits must be positive integers")
        if self.job_bytes > self.total_bytes or self.jobs > 1024:
            raise ValueError("invalid export storage or job budget")


class ExportJobs:
    def __init__(self, root: pathlib.Path, policy: ExportPolicy | None = None) -> None:
        self.root, self.policy = root, policy or ExportPolicy()
        self.directory = root / "exports"
        self.lock = threading.RLock()
        self.jobs: dict[str, dict[str, Any]] = {}
        self.cancel_events: dict[str, threading.Event] = {}
        self.downloads: set[str] = set()
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="iris-export")
        self.future: asyncio.Future | None = None
        self.closing = False
        self.storage_error: str | None = None
        try:
            self.directory.mkdir(exist_ok=True, mode=0o700)
        except OSError as exc:
            self.storage_error = str(exc)
            return
        # Persistent terminal statuses make interrupted exports observable after restart.
        try:
            folders = list(self.directory.iterdir())
        except OSError as exc:
            self.storage_error = str(exc)
            return
        for folder in folders:
            if not folder.is_dir() or len(folder.name) != 32:
                continue
            try:
                status_file = folder / "status.json"
                if status_file.stat().st_size > 64 * 1024:
                    raise ValueError("export status exceeds its metadata budget")
                job = json.loads(status_file.read_text())
                if (not isinstance(job, dict) or job.get("job_id") != folder.name
                        or not isinstance(job.get("created_ns"), int)
                        or job.get("status") not in {"queued", "running", "cancelling", "completed", "failed", "cancelled", "interrupted"}):
                    raise ValueError("invalid export status")
            except (OSError, ValueError):
                job = {"job_id": folder.name, "status": "failed", "created_ns": time.time_ns(),
                       "finished_ns": time.time_ns(), "error": "invalid export status metadata", "retain": False}
            if job["status"] in {"queued", "running", "cancelling"}:
                job.update(status="interrupted", error="Gateway stopped during export", finished_ns=time.time_ns())
            try:
                for path in folder.iterdir():
                    if path.is_dir():
                        shutil.rmtree(path)
                    elif path.name not in {"status.json", "bundle.zip"} or job["status"] != "completed" and path.name == "bundle.zip":
                        path.unlink()
                self._save(job)
            except OSError as exc:
                job["cleanup_error"] = str(exc)
            self.jobs[job["job_id"]] = job
        self.cleanup()

    def _save(self, job: dict[str, Any]) -> None:
        folder = self.directory / job["job_id"]
        temporary = folder / "status.tmp"
        temporary.write_text(json.dumps(job, ensure_ascii=False), encoding="utf-8")
        temporary.replace(folder / "status.json")

    def cleanup(self) -> None:
        with self.lock:
            cutoff = time.time_ns() - self.policy.ttl_seconds * 1_000_000_000
            for key, job in list(self.jobs.items()):
                if (key not in self.cancel_events and key not in self.downloads and not job.get("retain")
                        and job.get("finished_ns", job["created_ns"]) < cutoff):
                    try:
                        shutil.rmtree(self.directory / key)
                    except OSError as exc:
                        job["cleanup_error"] = str(exc)
                    else:
                        del self.jobs[key]

    def list(self) -> list[dict[str, Any]]:
        with self.lock:
            return [dict(item) for item in reversed(list(self.jobs.values()))]

    def get(self, key: str) -> dict[str, Any]:
        with self.lock:
            return dict(self.jobs[key])

    def submit(self, value: dict[str, Any]) -> dict[str, Any]:
        scope = export_scope(value)
        try:
            self.directory.mkdir(exist_ok=True, mode=0o700)
        except OSError as exc:
            self.storage_error = str(exc)
            raise StorageUnavailable(f"export storage unavailable: {exc}") from exc
        self.storage_error = None
        self.cleanup()
        with self.lock:
            if self.closing or self.cancel_events:
                raise ResourceExhausted("exports", 1)
            if len(self.jobs) >= self.policy.jobs:
                raise ResourceExhausted("retained export jobs; delete an old export", self.policy.jobs)
            used = sum(path.stat().st_size for path in self.directory.rglob("*") if path.is_file())
            maximum = min(self.policy.job_bytes, self.policy.total_bytes - used - 64 * 1024)
            if maximum < 1024 * 1024:
                raise ResourceExhausted("export disk bytes; delete an old export", self.policy.total_bytes)
            key = uuid.uuid4().hex
            folder = self.directory / key
            folder.mkdir(mode=0o700)
            job = {"job_id": key, "status": "queued", "scope": scope, "created_ns": time.time_ns(),
                   "retain": bool(scope.get("retain")), "bytes_written": 0, "input_bytes": 0, "files": 0,
                   "status_url": f"/v2/exports/{key}", "download_url": f"/v2/exports/{key}/download"}
            try:
                self._save(job)
            except BaseException:
                shutil.rmtree(folder)
                raise
            self.jobs[key] = job
            cancel = self.cancel_events[key] = threading.Event()
            self.future = asyncio.get_running_loop().run_in_executor(self.executor, self._run, key, scope, maximum, cancel)
            return dict(job)

    def _run(self, key: str, scope: dict[str, Any], maximum: int, cancel: threading.Event) -> None:
        target = self.directory / key / "bundle.zip"
        last_saved = 0.0
        def update(**changes):
            nonlocal last_saved
            with self.lock:
                self.jobs[key].update(changes)
                now = time.monotonic()
                if now - last_saved >= .25 or "status" in changes:
                    self._save(self.jobs[key])
                    last_saved = now
        store = None
        try:
            update(status="running")
            store = GatewayStore(self.root)
            store.db.execute("PRAGMA busy_timeout=100")
            store.exports.export_zip(target, scope=scope, cancel=cancel, maximum=maximum,
                                     progress=update, timeout=self.policy.timeout_seconds)
            digest = hashlib.sha256()
            with target.open("rb") as source:
                while True:
                    if cancel.is_set():
                        raise ExportCancelled("export cancelled")
                    chunk = source.read(CHUNK)
                    if not chunk:
                        break
                    digest.update(chunk)
            update(status="completed", phase="ready", bytes_written=target.stat().st_size,
                   sha256=digest.hexdigest(), finished_ns=time.time_ns(),
                   expires_ns=None if scope.get("retain") else time.time_ns() + self.policy.ttl_seconds * 1_000_000_000)
        except Exception as exc:  # noqa: BLE001 - background failures are reported through job status
            with contextlib.suppress(OSError):
                target.unlink(missing_ok=True)
            with self.lock:
                self.jobs[key].update(status="cancelled" if isinstance(exc, ExportCancelled) else "failed",
                                      error=str(exc), finished_ns=time.time_ns())
                # Keep the failure visible in memory even when the disk is full.
                with contextlib.suppress(OSError):
                    self._save(self.jobs[key])
        finally:
            if store is not None:
                store.close()
            with self.lock:
                self.cancel_events.pop(key, None)

    def cancel(self, key: str) -> dict[str, Any]:
        with self.lock:
            if key in self.cancel_events and self.jobs[key]["status"] in {"queued", "running", "cancelling"}:
                self.cancel_events[key].set()
                self.jobs[key]["status"] = "cancelling"
            return dict(self.jobs[key])

    def delete(self, key: str) -> None:
        with self.lock:
            if key in self.cancel_events or key in self.downloads:
                raise ResourceExhausted("active export or download", 1)
            if key not in self.jobs:
                raise KeyError(key)
            shutil.rmtree(self.directory / key)
            del self.jobs[key]

    async def download(self, request: web.Request) -> web.StreamResponse:
        key = request.match_info["job_id"]
        with self.lock:
            job = dict(self.jobs[key])
            if job["status"] != "completed":
                raise ValueError("export is not ready for download")
            if key in self.downloads:
                raise ResourceExhausted("downloads per export", 1)
            self.downloads.add(key)
        try:
            path = self.directory / key / "bundle.zip"
            response = web.StreamResponse(headers={"Content-Disposition": f'attachment; filename="esp-iris-{key}.zip"',
                                                    "Content-Type": "application/zip", "Content-Length": str(path.stat().st_size),
                                                    "X-Checksum-SHA256": job["sha256"]})
            await response.prepare(request)
            with path.open("rb") as source:
                while True:
                    chunk = await asyncio.get_running_loop().run_in_executor(None, source.read, CHUNK)
                    if not chunk:
                        break
                    await response.write(chunk)
            await response.write_eof()
            return response
        except (ConnectionError, asyncio.CancelledError):
            with self.lock:
                self.jobs[key]["last_download_error"] = "download disconnected; bundle remains available until expiry"
                with contextlib.suppress(OSError):
                    self._save(self.jobs[key])
            raise
        finally:
            with self.lock:
                self.downloads.discard(key)

    async def close(self) -> None:
        self.closing = True
        with self.lock:
            for cancel in self.cancel_events.values():
                cancel.set()
        if self.future is not None:
            await asyncio.shield(self.future)
        self.executor.shutdown(wait=False)


def register_exports(app: web.Application, service: Any) -> None:
    manager = ExportJobs(service.store.root, ExportPolicy(**service.store.get_setting("exports.policy", {})))
    service.exports = manager
    async def create(request):
        body = await request.json() if request.can_read_body else {}
        if not isinstance(body, dict):
            raise TypeError("export options must be an object")
        # Snapshot committed history in the worker; a busy ingress queue must
        # not delay accepting this background job.
        return web.json_response(manager.submit(body), status=202)
    async def listing(request):
        return web.json_response({"exports": manager.list()})
    async def status(request):
        return web.json_response(manager.get(request.match_info["job_id"]))
    async def cancel(request):
        return web.json_response(manager.cancel(request.match_info["job_id"]))
    async def delete(request):
        manager.delete(request.match_info["job_id"])
        return web.json_response({"deleted": True})
    async def upkeep():
        while True:
            await asyncio.sleep(60)
            await asyncio.get_running_loop().run_in_executor(None, manager.cleanup)
    async def lifecycle(app):
        task = asyncio.create_task(upkeep())
        try:
            yield
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await manager.close()
    app.cleanup_ctx.append(lifecycle)
    app.router.add_post("/v2/export", create)
    app.router.add_get("/v2/exports", listing)
    app.router.add_get("/v2/exports/{job_id}", status)
    app.router.add_post("/v2/exports/{job_id}/cancel", cancel)
    app.router.add_delete("/v2/exports/{job_id}", delete)
    app.router.add_get("/v2/exports/{job_id}/download", manager.download)
