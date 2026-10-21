from __future__ import annotations

import asyncio
import contextvars
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from types import SimpleNamespace

import pytest

from iris_gateway.compat import to_thread
from iris_gateway.link import SerialLink


def test_to_thread_preserves_context_variables() -> None:
    async def scenario() -> None:
        value = contextvars.ContextVar("value", default="missing")
        value.set("caller-context")
        assert await to_thread(value.get) == "caller-context"

    asyncio.run(scenario())


class FakeSerial:
    def __init__(self, reads: list[bytes]) -> None:
        self.reads = reads
        self.buffered = b""
        self.is_open = True
        self.calls = 0

    @property
    def in_waiting(self) -> int:
        return len(self.buffered)

    def read(self, size: int) -> bytes:
        self.calls += 1
        if not self.buffered:
            self.buffered = self.reads.pop(0)
        result = self.buffered[:size]
        self.buffered = self.buffered[size:]
        return result

    def close(self) -> None:
        self.is_open = False

    def write(self, data: bytes) -> int:
        return len(data)


def test_serial_timeout_is_not_treated_as_disconnect() -> None:
    async def scenario() -> None:
        serial = FakeSerial([b"", b"", b"frame\x00"])
        link = SerialLink("/dev/fake-iris", serial)
        assert await asyncio.wait_for(link.read(), 1.0) == b"frame\x00"
        assert serial.calls == 4
        await link.close()

    asyncio.run(scenario())


def test_serial_read_batches_already_buffered_frames() -> None:
    async def scenario() -> None:
        serial = FakeSerial([b"first\x00second\x00third\x00"])
        link = SerialLink("/dev/fake-iris", serial)
        assert await link.read() == b"first\x00second\x00third\x00"
        await link.close()

    asyncio.run(scenario())


def test_serial_write_is_not_blocked_by_an_in_flight_read() -> None:
    class DuplexSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__([])
            self.read_started = threading.Event()
            self.release_read = threading.Event()
            self.writes: list[bytes] = []

        def read(self, size: int) -> bytes:
            self.read_started.set()
            self.release_read.wait(0.5)
            return b"frame"

        def write(self, data: bytes) -> int:
            self.writes.append(data)
            return len(data)

    async def scenario() -> None:
        serial = DuplexSerial()
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        assert await to_thread(serial.read_started.wait, 0.5)
        await asyncio.wait_for(link.write(b"request"), 0.1)
        assert serial.writes == [b"request"]
        serial.release_read.set()
        assert await asyncio.wait_for(read_task, 1.0) == b"frame"
        await link.close()

    asyncio.run(scenario())


def test_serial_readers_do_not_starve_default_executor() -> None:
    class BlockingSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__([])
            self.started = threading.Event()
            self.release = threading.Event()

        def read(self, size: int) -> bytes:
            self.started.set()
            self.release.wait(2)
            return b"x"

    async def scenario() -> None:
        # A host with nine dual-CDC devices has more persistent readers than
        # the default executor's 16 workers on a 12-core Windows machine.
        asyncio.get_running_loop().set_default_executor(ThreadPoolExecutor(max_workers=1))
        serial = BlockingSerial()
        link = SerialLink("COM43", serial)
        reader = asyncio.create_task(link.read(1))
        try:
            for _ in range(100):
                if serial.started.is_set():
                    break
                await asyncio.sleep(0.001)
            assert serial.started.is_set()
            await asyncio.wait_for(link.write(b"request"), 0.5)
            assert await asyncio.wait_for(to_thread(lambda: "discovery"), 0.5) == "discovery"
        finally:
            serial.release.set()
            await reader
            await link.close()

    asyncio.run(scenario())


def test_serial_read_returns_eof_after_close() -> None:
    async def scenario() -> None:
        serial = FakeSerial([])
        serial.is_open = False
        link = SerialLink("/dev/fake-iris", serial)
        assert await link.read() == b""

    asyncio.run(scenario())


@pytest.mark.parametrize("application_cdc", [False, True])
def test_only_application_cdc_deasserts_dtr_on_close(application_cdc):
    async def scenario():
        serial = FakeSerial([])
        serial.dtr = True
        link = SerialLink("/dev/fake-iris", serial)
        link._release_dtr = application_cdc
        await link.close()
        assert serial.dtr is (not application_cdc)
        assert not serial.is_open
    asyncio.run(scenario())


def test_serial_close_waits_for_in_flight_read() -> None:
    class SlowSerial(FakeSerial):
        def read(self, size: int) -> bytes:
            time.sleep(0.02)
            return b""

    async def scenario() -> None:
        serial = SlowSerial([])
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        await asyncio.sleep(0)
        await link.close()
        assert await asyncio.wait_for(read_task, 1.0) == b""

    asyncio.run(scenario())


def test_serial_cancel_drains_in_flight_thread_before_close() -> None:
    class SlowSerial(FakeSerial):
        def read(self, size: int) -> bytes:
            time.sleep(0.02)
            return b""

    async def scenario() -> None:
        serial = SlowSerial([])
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        await asyncio.sleep(0)
        read_task.cancel()
        try:
            await read_task
        except asyncio.CancelledError:
            pass
        await link.close()
        assert serial.is_open is False

    asyncio.run(scenario())


def test_usb_serial_jtag_disables_hangup_on_close(monkeypatch) -> None:
    termios = pytest.importorskip("termios", reason="POSIX terminal control only")
    changed: list[tuple[int, int, list[object]]] = []

    class DeferredSerial(FakeSerial):
        def __init__(self, *, port: str | None, **options: object) -> None:
            super().__init__([])
            assert port is None
            self.is_open = False

        def open(self) -> None:
            assert self.port == "/dev/fake-usj"
            assert self.dtr is True and self.rts is True
            self.is_open = True

        def fileno(self) -> int:
            return 42

    attributes: list[object] = [0, 0, termios.HUPCL, 0, 0, 0, []]
    monkeypatch.setitem(
        sys.modules, "serial", SimpleNamespace(Serial=DeferredSerial)
    )
    monkeypatch.setattr(termios, "tcgetattr", lambda fd: attributes.copy())
    monkeypatch.setattr(
        termios,
        "tcsetattr",
        lambda fd, when, values: changed.append((fd, when, values)),
    )

    async def scenario() -> None:
        link = await SerialLink.open("/dev/fake-usj", hupcl=False)
        assert link._serial.dtr is False and link._serial.rts is False
        assert len(changed) == 1
        fd, when, values = changed[0]
        assert fd == 42
        assert when == termios.TCSANOW
        assert int(values[2]) & termios.HUPCL == 0
        await link.close()

    asyncio.run(scenario())
