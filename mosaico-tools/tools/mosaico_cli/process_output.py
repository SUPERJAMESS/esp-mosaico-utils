"""Complete disk output with bounded diagnostic and progress buffers."""
from __future__ import annotations

import codecs
import os
import signal
import subprocess
import time
from collections.abc import Callable
from pathlib import Path
from queue import Empty, Full, Queue
from threading import Event, Thread

TAIL_BYTES = 64 * 1024
CHUNK_BYTES = 16 * 1024
PROGRESS_ITEMS = 128


def stream_process(command: list[str], *, path: Path | None, timeout: float | None,
                   cwd: Path | None, env: dict[str, str] | None,
                   on_line: Callable[[str], None]) -> subprocess.CompletedProcess[str]:
    """stdout/exception.output is a 64 KiB tail; stdout_path contains every byte.

    Progress may skip old lines when its consumer is slow. This never discards
    the disk copy or blocks the child on UI work. No output file is created for
    sensitive commands. Cancellation and timeouts terminate and reap the child.
    """
    output = path.open("xb") if path is not None else None
    try:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, start_new_session=os.name == "posix")
    except BaseException:
        if output is not None:
            output.close()
        raise
    stdout = process.stdout
    assert stdout is not None
    lines: Queue[str] = Queue(maxsize=PROGRESS_ITEMS)
    done = Event()
    tail = bytearray()
    failures: list[BaseException] = []
    total = dropped = 0

    def publish(line: str) -> None:
        nonlocal dropped
        try:
            lines.put_nowait(line)
        except Full:
            try:
                lines.get_nowait()
                dropped += 1
            except Empty:
                pass
            lines.put_nowait(line)

    def read() -> None:
        nonlocal total
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        pending = ""
        try:
            while True:
                chunk = stdout.read1(CHUNK_BYTES)
                if not chunk:
                    break
                if output is not None:
                    output.write(chunk)
                total += len(chunk)
                tail.extend(chunk)
                del tail[:-TAIL_BYTES]
                pending += decoder.decode(chunk)
                while pending:
                    newline = pending.find("\n", 0, CHUNK_BYTES)
                    end = newline + 1 if newline >= 0 else CHUNK_BYTES if len(pending) >= CHUNK_BYTES else 0
                    if not end:
                        break
                    publish(pending[:end])
                    pending = pending[end:]
            pending += decoder.decode(b"", final=True)
            if pending:
                publish(pending)
        except BaseException as exc:  # noqa: BLE001 - re-raised by the owning thread
            failures.append(exc)
        finally:
            try:
                if output is not None:
                    output.close()
            except OSError as exc:
                failures.append(exc)
            stdout.close()
            done.set()

    reader = Thread(target=read, name="mosaico-output", daemon=True)
    reader.start()
    deadline = None if timeout is None else time.monotonic() + timeout

    def annotate(result):
        result.stdout_path = str(path) if path is not None else None
        result.stdout_truncated = total > TAIL_BYTES
        result.progress_dropped = dropped
        return result

    def diagnostic_tail() -> str:
        # Preserve subprocess text-mode newline behavior while the disk spool
        # retains the child's original bytes on every host.
        return tail.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")

    try:
        while not done.is_set() or not lines.empty():
            if failures:
                raise failures[0]
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                raise subprocess.TimeoutExpired(command, timeout)
            try:
                on_line(lines.get(timeout=0.05 if remaining is None else min(0.05, remaining)))
            except Empty:
                continue
        if failures:
            raise failures[0]
        remaining = None if deadline is None else max(0, deadline - time.monotonic())
        return annotate(subprocess.CompletedProcess(command, process.wait(timeout=remaining), diagnostic_tail()))
    except BaseException as exc:
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
        process.wait()
        reader.join(timeout=2)
        if isinstance(exc, subprocess.TimeoutExpired):
            exc.output = diagnostic_tail()
            annotate(exc)
        raise
    finally:
        reader.join(timeout=2)
