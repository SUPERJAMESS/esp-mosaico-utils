"""Process-owned stdout/stderr rotation, including inherited native writers."""
from __future__ import annotations

import contextlib
import os
import pathlib
import sys
import threading
import time


class RotatingLog:
    def __init__(self, path: pathlib.Path, *, max_bytes: int = 8 * 1024 * 1024,
                 backups: int = 3, max_days: int = 7) -> None:
        if max_bytes < 1 or not 0 <= backups <= 100 or max_days < 1:
            raise ValueError("invalid process log retention")
        self.path, self.max_bytes, self.backups = path, max_bytes, backups
        self.path.parent.mkdir(parents=True, exist_ok=True)
        cutoff = time.time() - max_days * 86400
        for index in range(backups + 1):
            old = self.path if index == 0 else self.path.with_name(f"{self.path.name}.{index}")
            if old.exists() and old.stat().st_mtime < cutoff:
                old.unlink()
        self.dropped_bytes = 0

    def write(self, data: bytes) -> None:
        # Bytes are chunked even when a child prints an arbitrarily long line.
        for offset in range(0, len(data), self.max_bytes):
            chunk = data[offset:offset + self.max_bytes]
            try:
                if self.path.exists() and self.path.stat().st_size + len(chunk) > self.max_bytes:
                    for index in range(self.backups, 0, -1):
                        source = self.path if index == 1 else self.path.with_name(f"{self.path.name}.{index - 1}")
                        if source.exists():
                            source.replace(self.path.with_name(f"{self.path.name}.{index}"))
                    if not self.backups:
                        self.path.unlink()
                with self.path.open("ab") as handle:
                    handle.write(chunk)
            except OSError:
                # Always drain the pipe: diagnostics must not block the process
                # when its filesystem fills or becomes read-only.
                self.dropped_bytes += len(chunk)


@contextlib.contextmanager
def redirect_process_log(path: pathlib.Path, *, max_bytes: int = 8 * 1024 * 1024):
    log = RotatingLog(path, max_bytes=max_bytes)
    saved = [os.dup(fd) for fd in (1, 2)]
    reader, writer = os.pipe()

    def drain() -> None:
        with os.fdopen(reader, "rb", buffering=0) as handle:
            while True:
                data = handle.read(65536)
                if not data:
                    break
                log.write(data)

    thread = threading.Thread(target=drain, name="iris-process-log", daemon=True)
    try:
        sys.stdout.flush()
        sys.stderr.flush()
        for fd in (1, 2):
            os.dup2(writer, fd)
        os.close(writer)
        thread.start()
        yield log
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        for fd, original in zip((1, 2), saved):
            os.dup2(original, fd)
            os.close(original)
        thread.join(timeout=2)
