from __future__ import annotations

import abc
import asyncio
import contextlib
import errno
import hashlib
import os
import pathlib
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Protocol

from .compat import to_thread


class SerialPort(Protocol):
    @property
    def is_open(self) -> bool: ...

    @property
    def in_waiting(self) -> int: ...

    def read(self, size: int) -> bytes: ...

    def write(self, data: bytes) -> int | None: ...

    def close(self) -> None: ...


class Link(abc.ABC):
    endpoint: str
    console: bool = False

    @abc.abstractmethod
    async def read(self, size: int = 4096) -> bytes: ...

    @abc.abstractmethod
    async def write(self, data: bytes) -> None: ...

    @abc.abstractmethod
    async def close(self) -> None: ...


class TcpLink(Link):
    def __init__(
        self,
        host: str,
        port: int,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        *,
        console: bool = True,
    ) -> None:
        self.host = host
        self.port = port
        self.endpoint = f"tcp:{host}:{port}"
        self._reader = reader
        self._writer = writer
        self.console = console

    @classmethod
    async def open(cls, host: str, port: int, *, console: bool = True) -> TcpLink:
        reader, writer = await asyncio.open_connection(host, port)
        return cls(host, port, reader, writer, console=console)

    async def read(self, size: int = 4096) -> bytes:
        return await self._reader.read(size)

    async def write(self, data: bytes) -> None:
        self._writer.write(data)
        await self._writer.drain()

    async def close(self) -> None:
        self._writer.close()
        await self._writer.wait_closed()


class SerialLink(Link):
    def __init__(
        self, port: str, serial_port: SerialPort, *, endpoint: str | None = None,
        console: bool = True,
    ) -> None:
        self.port = port
        self.console = console
        # Discovery prefers a stable by-path identity. Keep that identity
        # instead of resolving it to a transient tty/COM endpoint so the same
        # supervisor survives firmware re-enumeration.
        self.endpoint = endpoint or f"usb:{port}"
        self._serial = serial_port
        self.reader_started = asyncio.Event()
        self._read_lock = asyncio.Lock()
        self._write_lock = asyncio.Lock()
        self._close_lock = asyncio.Lock()
        self._closing = False
        self._release_dtr = False
        # Persistent CDC readers must not occupy the default executor used by
        # writes, discovery and cancellation. Nine dual-CDC boards exceed the
        # default pool on common Windows hosts.
        self._read_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="iris-serial-read")

    @classmethod
    async def open(
        cls,
        port: str,
        *,
        hupcl: bool | None = None,
        endpoint: str | None = None,
        console: bool = True,
        assert_dtr: bool = False,
        baudrate: int = 115200,
    ) -> SerialLink:
        import serial

        def open_port() -> SerialPort:
            serial_port = serial.Serial(
                port=None,
                baudrate=baudrate,
                timeout=0.2,
                write_timeout=2,
                exclusive=True if os.name == "posix" else None,
            )
            try:
                # Match IDF Monitor's no-reset attach sequence. Opening with
                # deasserted outputs lets the OS's initial assertion create a
                # reset edge as pySerial applies DTR before RTS. Assert both
                # before open, then release RTS first and DTR second.
                serial_port.dtr = True
                serial_port.rts = True
                serial_port.port = port
                serial_port.open()
                serial_port.rts = False
                serial_port.dtr = False
                if assert_dtr:
                    serial_port.dtr = True
                if hupcl is not None and sys.platform != "win32":
                    import termios

                    attributes = termios.tcgetattr(serial_port.fileno())
                    if hupcl:
                        attributes[2] |= termios.HUPCL
                    else:
                        attributes[2] &= ~termios.HUPCL
                    termios.tcsetattr(
                        serial_port.fileno(), termios.TCSANOW, attributes
                    )
            except BaseException:
                serial_port.close()
                raise
            return serial_port

        serial_port = await to_thread(open_port)
        link = cls(port, serial_port, endpoint=endpoint, console=console)
        link._release_dtr = assert_dtr
        return link

    def _read_batch(self, size: int) -> bytes:
        if size <= 0:
            return b""
        data = self._serial.read(1)
        if not data or size == 1:
            return data
        waiting = int(getattr(self._serial, "in_waiting", 0) or 0)
        if waiting <= 0:
            return data
        return data + self._serial.read(min(waiting, size - len(data)))

    async def read(self, size: int = 65536) -> bytes:
        while True:
            async with self._read_lock:
                if self._closing or not self._serial.is_open:
                    return b""
                read_task = asyncio.get_running_loop().run_in_executor(
                    self._read_executor, self._read_batch, size
                )
                self.reader_started.set()
                try:
                    data = await asyncio.shield(read_task)
                except asyncio.CancelledError:
                    cancel_read = getattr(self._serial, "cancel_read", None)
                    if cancel_read is not None:
                        await to_thread(cancel_read)
                    with contextlib.suppress(Exception):
                        await read_task
                    raise
            if data:
                return data
            if self._closing:
                return b""
            await asyncio.sleep(0)

    async def write(self, data: bytes) -> None:
        async with self._write_lock:
            if self._closing or not self._serial.is_open:
                raise ConnectionError("ESP-Iris serial link is closed")
            written = await to_thread(self._serial.write, data)
        if written != len(data):
            raise OSError(f"short serial write: {written}/{len(data)}")

    async def hardware_reset(self, mode: str, circuit: str, *, chip: str | None = None) -> None:
        from .hardware_reset import execute_reset, reset_steps
        steps = reset_steps(mode, circuit, chip=chip)
        await asyncio.wait_for(self.reader_started.wait(), 2.0)
        async with self._write_lock:
            if self._closing or not self._serial.is_open:
                raise ConnectionError("serial endpoint closed before reset")
            task = asyncio.create_task(to_thread(execute_reset, self._serial, steps))
            try:
                await asyncio.shield(task)
            except asyncio.CancelledError:
                await task
                raise

    async def close(self) -> None:
        async with self._close_lock:
            if not self._serial.is_open:
                self._read_executor.shutdown(wait=False)
                return
            self._closing = True
            cancel_read = getattr(self._serial, "cancel_read", None)
            if cancel_read is not None:
                with contextlib.suppress(Exception):
                    await to_thread(cancel_read)
            async with self._read_lock, self._write_lock:
                if self._serial.is_open:
                    try:
                        # Application CDC uses DTR as its session lifetime.
                        # HUPCL is disabled to avoid native UART/USJ resets,
                        # so release this opt-in CDC signal explicitly.
                        if self._release_dtr:
                            await to_thread(setattr, self._serial, "dtr", False)
                    finally:
                        try:
                            await to_thread(self._serial.close)
                        finally:
                            self._read_executor.shutdown(wait=False)


class EndpointLock:
    """Cross-process advisory lock for one physical endpoint."""

    @staticmethod
    def held(endpoint: str, root: pathlib.Path) -> bool:
        """Probe an existing lock without creating files or changing owner metadata."""
        path = root / f"{hashlib.sha256(endpoint.encode()).hexdigest()}.lock"
        try:
            stream = path.open("r+b")
        except FileNotFoundError:
            return False
        with stream:
            if sys.platform == "win32":
                import msvcrt
                try:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                except OSError:
                    return True
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                try:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    return True
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        return False

    def __init__(self, endpoint: str, *, root: pathlib.Path | None = None) -> None:
        digest = hashlib.sha256(endpoint.encode()).hexdigest()
        root = root or pathlib.Path(tempfile.gettempdir()) / "esp-iris-locks"
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.path = root / f"{digest}.lock"
        # Windows supports locking ranges beyond EOF. Do not initialize a byte
        # here: another owner may hold the lock while replacing its metadata.
        self._file = self.path.open("a+b")

    def _owner_message(self) -> str:
        try:
            self._file.seek(0)
            owner = self._file.read(512).decode("utf-8", errors="replace").strip()
        except OSError:
            # Windows byte-range locks can also prohibit reading this byte.
            owner = "owner metadata unavailable"
        return f"endpoint is owned by another ESP-Iris instance ({owner}; lock={self.path})"

    def acquire(self, *, blocking: bool = False) -> None:
        if sys.platform == "win32":
            import msvcrt

            self._file.seek(0)
            while True:
                try:
                    msvcrt.locking(self._file.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError as exc:
                    if not blocking or exc.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                        raise RuntimeError(self._owner_message()) from exc
                    # LK_LOCK gives up after ten one-second retries. Match
                    # POSIX blocking semantics for serialized CLI admission.
                    time.sleep(0.05)
        else:
            import fcntl

            try:
                flags = fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB)
                fcntl.flock(self._file.fileno(), flags)
            except OSError as exc:
                raise RuntimeError(
                    self._owner_message()
                ) from exc
        self._file.seek(0)
        self._file.truncate()
        self._file.write(f"pid={os.getpid()}\nendpoint={self.path.name}\n".encode())
        self._file.flush()

    def close(self) -> None:
        if self._file.closed:
            return
        if sys.platform == "win32":
            import msvcrt

            self._file.seek(0)
            try:
                msvcrt.locking(  # type: ignore[attr-defined]
                    self._file.fileno(), msvcrt.LK_UNLCK, 1  # type: ignore[attr-defined]
                )
            except OSError:
                pass
        else:
            import fcntl

            fcntl.flock(self._file.fileno(), fcntl.LOCK_UN)
        self._file.close()

    def fileno(self) -> int:
        """Allow a supervised POSIX writer to retain this lock if its parent dies."""
        return self._file.fileno()
