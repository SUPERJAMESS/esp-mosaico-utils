from __future__ import annotations

import asyncio
import contextlib
import hashlib
import hmac
import struct
import time
import zlib
from types import SimpleNamespace

import pytest

import iris_gateway.session as session_module
from iris_gateway.protocol import (
    Capability,
    Channel,
    ControlType,
    CrashType,
    EventType,
    Frame,
    MediaType,
    OtaType,
    ProtocolError,
    TlvTag,
    decode_frame,
    encode_frame,
    encode_tlv,
)
from iris_gateway.session import DeviceSession


def test_status_decodes_spiram_watermarks_and_old_firmware() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        session = DeviceSession(FakeLink(), discard, discard)
        session.info = SimpleNamespace(
            hardware_mac="",
            as_dict=lambda: {"device_id": "device-1", "boot_id": "42"},
        )
        payload = encode_tlv([
            (TlvTag.FREE_INTERNAL, struct.pack("<I", 600)),
            (TlvTag.MIN_FREE_INTERNAL, struct.pack("<I", 400)),
            (TlvTag.TOTAL_INTERNAL, struct.pack("<I", 1000)),
            (TlvTag.TOTAL_SPIRAM, struct.pack("<I", 2000)),
            (TlvTag.FREE_SPIRAM, struct.pack("<I", 1500)),
            (TlvTag.MIN_FREE_SPIRAM, struct.pack("<I", 1000)),
        ])

        async def response(channel: int, type_: int) -> Frame:
            assert channel == Channel.CONTROL
            assert type_ == ControlType.STATUS_REQUEST
            return Frame(channel=channel, type=ControlType.STATUS_RESPONSE, payload=payload)

        session._request = response
        status = await session.status()
        assert (status["total_spiram"], status["free_spiram"], status["min_free_spiram"]) == (2000, 1500, 1000)

        payload = b""
        old_status = await session.status()
        assert old_status["total_spiram"] == 0
        assert old_status["min_free_spiram"] == 0

    asyncio.run(scenario())


def test_task_memory_snapshot_is_complete_and_capability_gated() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        session = DeviceSession(FakeLink(), discard, discard)
        session.info = SimpleNamespace(
            capabilities=Capability.TASK_MEMORY,
            as_dict=lambda: {"device_id": "device-1", "boot_id": "42"},
        )

        async def response(channel: int, type_: int) -> Frame:
            assert channel == Channel.CONTROL
            assert type_ == ControlType.TASKS_REQUEST
            return Frame(
                channel=channel,
                type=ControlType.TASKS_RESPONSE,
                payload=struct.pack("<QHHIIII", 12345, 2, 0, 17, 384, 18, 768),
            )

        session._request = response
        snapshot = await session.task_memory()
        assert snapshot["uptime_us"] == 12345
        assert snapshot["tasks"] == [
            {"task_number": 17, "stack_free_min_bytes": 384},
            {"task_number": 18, "stack_free_min_bytes": 768},
        ]

        async def truncated(channel: int, type_: int) -> Frame:
            frame = await response(channel, type_)
            frame.payload = frame.payload[:-1]
            return frame

        session._request = truncated
        with pytest.raises(ProtocolError, match="invalid task memory response"):
            await session.task_memory()

        session.info.capabilities = 0
        with pytest.raises(NotImplementedError):
            await session.task_memory()

    asyncio.run(scenario())


class FakeLink:
    endpoint = "fake:test"

    def __init__(self) -> None:
        self.incoming: asyncio.Queue[bytes] = asyncio.Queue()
        self.writes: list[bytes] = []
        self.closed = False

    async def read(self, size: int = 4096) -> bytes:
        return await self.incoming.get()

    async def write(self, data: bytes) -> None:
        self.writes.append(data)
        frame = decode_frame(data[:-1])
        if frame.channel == Channel.CONTROL and frame.type == ControlType.HELLO_ACK and len(frame.payload) == 17 and not frame.flags & (1 << 5):
            await self.incoming.put(encode_frame(Frame(
                channel=Channel.CONTROL, type=ControlType.AUTH_RESULT,
                session_id=frame.session_id, sequence=0, payload=b"\x01")))

    async def close(self) -> None:
        self.closed = True


def test_media_credit_does_not_block_receive_behind_rpc_writer() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        link = FakeLink()
        session = DeviceSession(link, discard, discard)
        session.info = SimpleNamespace(device_id="device", boot_id=42, session_id=7)
        # Model a large RPC waiting for the device's RX while mirror TX is full.
        await session._write_lock.acquire()
        payload = struct.pack("<QIIHHHHHHIHH", 0, 1, 0, 0, 2,
                              0, 0, 1, 1, 2, 1, 0) + b"\x00\x00"
        frame = Frame(channel=Channel.SCREEN, type=MediaType.DATA, payload=payload)
        for _ in range(20):
            await asyncio.wait_for(session._handle_media(frame), 0.1)
        assert len(session._credit_tasks) == 1  # bounded, coalesced replenishment
        assert not link.writes
        session._write_lock.release()
        await asyncio.gather(*session._credit_tasks.values())
        assert not session._credit_tasks
        grants = [decode_frame(wire[:-1]) for wire in link.writes]
        assert len(grants) == 1
        assert grants[0].type == ControlType.CREDIT
        assert session._media_credit[int(Channel.SCREEN)] == 128 * 1024
        await session.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("yield_before_close", [False, True])
def test_credit_tasks_are_cancelled_on_close_and_write_failure(yield_before_close) -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        link = FakeLink()
        session = DeviceSession(link, discard, discard)
        await session._write_lock.acquire()
        session._queue_credit(Channel.LOG, 1024)
        session._queue_credit(Channel.SCREEN, 1024)
        if yield_before_close:
            await asyncio.sleep(0)
        await asyncio.wait_for(session.close(), 0.1)
        assert not session._credit_tasks
        assert link.closed
        session._write_lock.release()

        session = DeviceSession(FakeLink(), discard, discard)
        async def fail(*args, **kwargs) -> None:
            raise OSError("write failed")
        session._send = fail
        session._queue_credit(Channel.SCREEN, 1024)
        await asyncio.wait_for(asyncio.gather(*session._credit_tasks.values()), 0.1)
        assert not session._credit_tasks
        assert session._closed and session.link.closed

    asyncio.run(scenario())


async def wait_for_request(
    link: FakeLink, channel: int, type_: int, after: int = 0
) -> tuple[int, Frame]:
    for _ in range(100):
        for index, wire in enumerate(link.writes[after:], start=after):
            frame = decode_frame(wire[:-1])
            if frame.channel == channel and frame.type == type_:
                return index, frame
        await asyncio.sleep(0)
    raise AssertionError(f"request {channel}/{type_} was not sent")


def test_send_sequence_wraps_uint32_in_wire_order_and_new_session_starts_fresh() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        link = FakeLink()
        session = DeviceSession(link, discard, discard)
        session._sequence[Channel.CONTROL] = 0xFFFFFFFD
        await asyncio.gather(*(
            session._send(Channel.CONTROL, ControlType.PING) for _ in range(4)
        ))
        assert [decode_frame(wire[:-1]).sequence for wire in link.writes] == [
            0xFFFFFFFE, 0xFFFFFFFF, 0, 1
        ]
        replacement = DeviceSession(link, discard, discard)
        await replacement._send(Channel.CONTROL, ControlType.PING)
        assert decode_frame(link.writes[-1][:-1]).sequence == 1

    asyncio.run(scenario())


def test_request_cannot_start_after_readiness_wait_when_session_closed() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        link = FakeLink()
        session = DeviceSession(link, discard, discard)

        async def close_during_ready(timeout: float = 5.0):
            session._closed = True

        session.wait_ready = close_during_ready
        with pytest.raises(ConnectionError, match="session closed"):
            await session._request_unlocked(Channel.CONTROL, ControlType.PING)
        assert not session._pending
        assert not link.writes

    asyncio.run(scenario())


def test_cancellation_during_eof_cleanup_propagates_and_closes_link() -> None:
    async def scenario() -> None:
        async def discard(value: object) -> None:
            pass

        link = FakeLink()
        session = DeviceSession(link, discard, discard)
        cleanup_started = asyncio.Event()

        async def slow_clock() -> None:
            try:
                await asyncio.Future()
            finally:
                cleanup_started.set()
                await asyncio.Future()

        session._clock_task = asyncio.create_task(slow_clock())
        await asyncio.sleep(0)
        task = asyncio.create_task(session.run())
        await link.incoming.put(b"")
        await asyncio.wait_for(cleanup_started.wait(), 1)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, 1)
        assert link.closed
        assert session._clock_task.done()

    asyncio.run(scenario())


def test_session_hello_credit_and_log_event() -> None:
    async def scenario() -> None:
        link = FakeLink()
        ready: list[str] = []
        events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            assert session.info is not None
            ready.append(session.info.device_id)

        async def on_event(event: dict[str, object]) -> None:
            events.append(event)

        session = DeviceSession(link, on_ready, on_event)
        task = asyncio.create_task(session.run())
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, bytes.fromhex("00112233445566778899aabbccddeeff")),
                (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 2)),
                (TlvTag.LINK_ROLE, b"\x01"),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x400F)),
                (TlvTag.TRANSPORT, b"\x01"),
                (TlvTag.AUTH_MODE, b"\x00"),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        info = await session.wait_ready()
        assert info.device_id == "00112233445566778899aabbccddeeff"
        assert info.as_dict()["ota_project_name_match_required"] is True
        assert "ota_project_name_match" in info.as_dict()["capability_names"]
        assert ready == [info.device_id]
        sent_types = [decode_frame(wire[:-1]).type for wire in link.writes]
        assert sent_types[:2] == [ControlType.HELLO_ACK, ControlType.CREDIT]

        writes_before_duplicate = len(link.writes)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=info.session_id,
                    sequence=2,
                    payload=hello,
                )
            )
        )
        for _ in range(20):
            if len(link.writes) > writes_before_duplicate:
                break
            await asyncio.sleep(0)
        duplicate_types = [decode_frame(wire[:-1]).type for wire in link.writes]
        assert ready == [info.device_id]
        assert duplicate_types.count(ControlType.HELLO_ACK) == 2
        assert duplicate_types.count(ControlType.CREDIT) == 1

        log_payload = struct.pack("<QIBBH", 1234, 0, 1, 0, 5) + b"hello"
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.LOG,
                    type=1,
                    session_id=info.session_id,
                    sequence=1,
                    payload=log_payload,
                )
            )
        )
        for _ in range(20):
            if events:
                break
            await asyncio.sleep(0)
        assert events[0]["text"] == "hello"
        assert events[0]["monotonic_us"] == 1234

        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_ota_end_timeout_is_reconciled_with_device_job() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        session._console = False
        calls = 0

        async def request(channel, type_, payload=b"", timeout=10.0):
            nonlocal calls
            del payload, timeout
            calls += 1
            if calls == 1:
                assert (channel, type_) == (Channel.OTA, OtaType.BEGIN)
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 77, 1, 1, 5) + b"ota_1",
                )
            if calls == 2:
                assert (channel, type_) == (Channel.OTA, OtaType.DATA)
                return Frame(
                    channel=channel,
                    type=OtaType.DATA_RESPONSE,
                    payload=struct.pack("<IHH", 1, 900, 0),
                )
            if calls == 3:
                assert (channel, type_) == (Channel.OTA, OtaType.END)
                raise TimeoutError
            assert (channel, type_) == (Channel.CONTROL, ControlType.JOB_QUERY)
            return Frame(
                channel=channel,
                type=ControlType.JOB_STATUS,
                payload=struct.pack("<IHBBHHi", 77, 0x100, 2, 0, 1000, 0, 0),
            )

        session._request = request
        session._request_unlocked = request
        session._request_lock = asyncio.Lock()
        result = await session.ota_update(b"x", timeout=0.01)
        assert result["completion_evidence"] == "device_job"
        assert result["job_id"] == 77

    asyncio.run(scenario())


def test_ota_begin_uses_extended_timeout_for_full_partition_erase() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        session._console = False
        session._request_lock = asyncio.Lock()
        calls: list[tuple[int, int, float]] = []

        async def request(channel, type_, payload=b"", timeout=10.0):
            del payload
            calls.append((channel, type_, timeout))
            if type_ == OtaType.BEGIN:
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 81, 1, 1, 5) + b"ota_0",
                )
            if type_ == OtaType.END:
                return Frame(
                    channel=channel,
                    type=OtaType.END_RESPONSE,
                    payload=struct.pack("<Ii", 81, 0),
                )
            raise AssertionError(f"unexpected OTA request type {type_}")

        async def request_unlocked(channel, type_, payload=b"", timeout=10.0):
            calls.append((channel, type_, timeout))
            assert type_ == OtaType.DATA
            offset = struct.unpack_from("<I", payload)[0]
            return Frame(
                channel=channel,
                type=OtaType.DATA_RESPONSE,
                payload=struct.pack("<IHH", offset + 1, 900, 0),
            )

        session._request = request
        session._request_unlocked = request_unlocked
        await session.ota_update(b"x", timeout=0.01)

        assert calls == [
            (Channel.OTA, OtaType.BEGIN, DeviceSession.OTA_BEGIN_TIMEOUT_SECONDS),
            (Channel.OTA, OtaType.DATA, 0.01),
            (Channel.OTA, OtaType.END, 0.01),
        ]

    asyncio.run(scenario())


def test_ota_end_session_close_is_deferred_to_gateway_reconnect_validation() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        session._console = False
        calls = 0

        async def request(channel, type_, payload=b"", timeout=10.0):
            nonlocal calls
            del payload, timeout
            calls += 1
            if calls == 1:
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 78, 1, 1, 5) + b"ota_1",
                )
            if calls == 2:
                return Frame(
                    channel=channel,
                    type=OtaType.DATA_RESPONSE,
                    payload=struct.pack("<IHH", 1, 900, 0),
                )
            assert (channel, type_) == (Channel.OTA, OtaType.END)
            raise ConnectionError("ESP-Iris session closed")

        session._request = request
        session._request_unlocked = request
        session._request_lock = asyncio.Lock()
        result = await session.ota_update(b"x", timeout=0.01)
        assert result["completion_evidence"] == "session_close"
        assert result["job_id"] == 78

    asyncio.run(scenario())


def test_ota_waits_for_flash_ack_and_resumes_from_confirmed_offset() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        session._console = False
        session._request_lock = asyncio.Lock()
        attempts: dict[int, int] = {}
        in_flight = 0

        async def request(channel, type_, payload=b"", timeout=10.0):
            del payload, timeout
            if (channel, type_) == (Channel.OTA, OtaType.BEGIN):
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 79, 4, 1, 5) + b"ota_1",
                )
            if (channel, type_) == (Channel.OTA, OtaType.STATUS):
                return Frame(
                    channel=channel,
                    type=OtaType.STATUS,
                    payload=struct.pack("<IIIHBBi", 79, 4, 1, 225, 1, 5, 0) + b"ota_1",
                )
            assert (channel, type_) == (Channel.OTA, OtaType.END)
            return Frame(
                channel=channel,
                type=OtaType.END_RESPONSE,
                payload=struct.pack("<Ii", 79, 0),
            )

        async def request_unlocked(channel, type_, payload=b"", timeout=10.0):
            nonlocal in_flight
            del timeout
            assert (channel, type_) == (Channel.OTA, OtaType.DATA)
            in_flight += 1
            # Real firmware rejects another write while its executor is busy.
            assert in_flight == 1
            await asyncio.sleep(0)
            in_flight -= 1
            offset = struct.unpack_from("<I", payload)[0]
            attempts[offset] = attempts.get(offset, 0) + 1
            if offset == 1 and attempts[offset] == 1:
                await asyncio.sleep(0)
                raise TimeoutError
            return Frame(
                channel=channel,
                type=OtaType.DATA_RESPONSE,
                payload=struct.pack("<IHH", offset + 1, (offset + 1) * 225, 0),
            )

        session._request = request
        session._request_unlocked = request_unlocked
        result = await session.ota_update(b"abcd", timeout=0.01)
        assert result["job_id"] == 79
        assert attempts == {0: 1, 1: 2, 2: 1, 3: 1}

    asyncio.run(scenario())


def test_ota_data_window_rejects_a_truncated_response_batch(monkeypatch) -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        session._console = False
        session._request_lock = asyncio.Lock()

        async def request(channel, type_, payload=b"", timeout=10.0):
            del payload, timeout
            assert (channel, type_) == (Channel.OTA, OtaType.BEGIN)
            return Frame(
                channel=channel,
                type=OtaType.BEGIN_RESPONSE,
                payload=struct.pack("<IIHB", 80, 1, 1, 5) + b"ota_1",
            )

        async def request_unlocked(channel, type_, payload=b"", timeout=10.0):
            del payload, timeout
            return Frame(channel=channel, type=type_, payload=b"")

        original_gather = asyncio.gather

        async def truncated_gather(*tasks, **kwargs):
            del kwargs
            for task in tasks:
                task.cancel()
            await original_gather(*tasks, return_exceptions=True)
            return []

        session._request = request
        session._request_unlocked = request_unlocked
        monkeypatch.setattr(session_module.asyncio, "gather", truncated_gather)
        with pytest.raises(ProtocolError, match="response count"):
            await session.ota_update(b"x", timeout=0.01)

    asyncio.run(scenario())


def test_event_time_fields_crash_metadata_and_chunk_download() -> None:
    async def scenario() -> None:
        link = FakeLink()
        events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            events.append(event)

        session = DeviceSession(link, on_ready, on_event)
        run_task = asyncio.create_task(session.run())
        device_id = bytes.fromhex("00112233445566778899aabbccddeeff")
        firmware_sha = bytes.fromhex("ab" * 32)
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, device_id),
                (TlvTag.BOOT_ID, struct.pack("<Q", 77)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 2)),
                (TlvTag.LINK_ROLE, b"\x01"),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x4F)),
                (TlvTag.TRANSPORT, b"\x01"),
                (TlvTag.AUTH_MODE, b"\x00"),
                (TlvTag.FIRMWARE_SHA256, firmware_sha),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        info = await session.wait_ready()
        assert session._clock_task is not None
        session._clock_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await session._clock_task
        session._clock_task = None

        event_payload = encode_tlv(
            [
                (TlvTag.BOOT_ID, struct.pack("<Q", info.boot_id)),
                (TlvTag.UPTIME_US, struct.pack("<Q", 12345)),
                (TlvTag.RESET_REASON, struct.pack("<I", 9)),
            ]
        )
        event_wire = encode_frame(
            Frame(
                channel=Channel.EVENT,
                type=EventType.BOOT,
                session_id=info.session_id,
                sequence=1,
                payload=event_payload,
            )
        )
        await link.incoming.put(event_wire)
        await link.incoming.put(event_wire)
        for _ in range(20):
            if events:
                break
            await asyncio.sleep(0)
        assert len(events) == 1
        assert events[0]["session_id"] == info.session_id
        assert events[0]["host_receive_monotonic_ns"]
        assert events[0]["host_receive_wall_ns"]
        assert events[0]["event_id"].endswith(":12345:1")

        report_task = asyncio.create_task(session.crash_report())
        for _ in range(20):
            if any(
                decode_frame(wire[:-1]).channel == Channel.CRASH
                for wire in link.writes
            ):
                break
            await asyncio.sleep(0)
        request = next(
            decode_frame(wire[:-1])
            for wire in reversed(link.writes)
            if decode_frame(wire[:-1]).channel == Channel.CRASH
        )
        assert request.type == CrashType.METADATA_REQUEST
        metadata = encode_tlv(
            [
                (TlvTag.BOOT_ID, struct.pack("<Q", info.boot_id)),
                (TlvTag.RESET_REASON, struct.pack("<I", 9)),
                (TlvTag.PREVIOUS_BOOT_CRASH, b"\x01"),
                (TlvTag.CORE_DUMP_PRESENT, b"\x01"),
                (TlvTag.CORE_DUMP_VALID, b"\x01"),
                (TlvTag.CORE_DUMP_SIZE, struct.pack("<I", 4)),
                (TlvTag.CORE_DUMP_CHUNK_MAX, struct.pack("<I", 512)),
                (TlvTag.CORE_DUMP_ELF_SHA256, b"ab" * 32),
                (TlvTag.CORE_DUMP_ELF_SHA256_COMPLETE, b"\x01"),
                (TlvTag.PANIC_REASON, b"Load access fault"),
                (TlvTag.CRASH_COUNT, struct.pack("<I", 3)),
                (TlvTag.CRASH_LIMIT, struct.pack("<I", 3)),
                (TlvTag.CRASH_LOOP_TRIGGERED, b"\x01"),
                (TlvTag.CRASH_RECOVERY_PENDING, b"\x01"),
                (TlvTag.CRASH_ORIGIN_RESET_REASON, struct.pack("<I", 9)),
                (TlvTag.CRASH_FAILED_APP_ADDRESS, struct.pack("<I", 0x120000)),
                (TlvTag.CRASH_FAILED_BOOT_ID, struct.pack("<Q", 123456789)),
                (TlvTag.CRASH_FAILED_FIRMWARE_SHA256, bytes.fromhex("ab" * 32)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CRASH,
                    type=CrashType.METADATA_RESPONSE,
                    flags=1,
                    session_id=info.session_id,
                    request_id=request.request_id,
                    sequence=1,
                    payload=metadata,
                )
            )
        )
        report = await report_task
        assert report["previous_boot_crash"] is True
        assert report["decode_eligible"] is True
        assert report["core_dump_chunk_max"] == 512
        assert report["panic_reason"] == "Load access fault"
        assert report["crash_count"] == 3
        assert report["crash_limit"] == 3
        assert report["crash_loop_triggered"] is True
        assert report["crash_recovery_pending"] is True
        assert report["crash_origin_reset_reason"] == 9
        assert report["crash_failed_app_address"] == 0x120000
        assert report["crash_failed_boot_id"] == 123456789
        assert report["crash_failed_firmware_sha256"] == "ab" * 32

        chunk_task = asyncio.create_task(session.read_core_dump_chunk(0, 1024))
        for _ in range(20):
            crash_requests = [
                decode_frame(wire[:-1])
                for wire in link.writes
                if decode_frame(wire[:-1]).channel == Channel.CRASH
            ]
            if len(crash_requests) >= 2:
                break
            await asyncio.sleep(0)
        chunk_request = crash_requests[-1]
        assert struct.unpack("<IHH", chunk_request.payload) == (0, 512, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CRASH,
                    type=CrashType.READ_RESPONSE,
                    flags=1 | 16,
                    session_id=info.session_id,
                    request_id=chunk_request.request_id,
                    sequence=2,
                    payload=struct.pack("<II", 0, 4) + b"core",
                )
            )
        )
        assert await chunk_task == (4, b"core")

        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

    asyncio.run(scenario())


def test_pairing_challenge_hmac_gates_session_ready() -> None:
    async def scenario() -> None:
        link = FakeLink()
        token = bytes.fromhex("11" * 32)
        challenge = bytes.fromhex("22" * 32)
        device_id = bytes.fromhex("00112233445566778899aabbccddeeff")
        boot_id = 91
        session_id = 0x87654321

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link, on_ready, on_event, pairing_token=token
        )
        task = asyncio.create_task(session.run())
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, device_id),
                (TlvTag.BOOT_ID, struct.pack("<Q", boot_id)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 2)),
                (TlvTag.LINK_ROLE, b"\x01"),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FF)),
                (TlvTag.TRANSPORT, b"\x02"),
                (TlvTag.AUTH_MODE, b"\x01"),
                (TlvTag.AUTH_CHALLENGE, challenge),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=session_id,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        _, proof_frame = await wait_for_request(
            link, Channel.CONTROL, ControlType.HELLO_ACK
        )
        assert len(proof_frame.payload) == 65
        binding = proof_frame.payload[:17]
        assert binding == b"\x01" + session.owner_id
        nonce = proof_frame.payload[17:33]
        expected = hmac.new(
            token,
            b"ESP-Iris-auth-0.2"
            + device_id
            + struct.pack("<QI", boot_id, session_id)
            + challenge
            + nonce
            + binding,
            hashlib.sha256,
        ).digest()
        assert hmac.compare_digest(proof_frame.payload[33:], expected)
        assert not session._ready.is_set()
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.AUTH_RESULT,
                    flags=1,
                    session_id=session_id,
                    sequence=1,
                    payload=b"\x01",
                )
            )
        )
        info = await session.wait_ready()
        assert info.auth_mode == 1
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_missing_pairing_token_has_the_same_minimum_failure_delay() -> None:
    async def scenario() -> None:
        link = FakeLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(link, on_ready, on_event)
        resolution = time.get_clock_info("monotonic").resolution
        delay = max(0.01, 4 * resolution)
        session.AUTH_MISSING_TOKEN_DELAY_SECONDS = delay
        task = asyncio.create_task(session.run())
        started = asyncio.get_running_loop().time()
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x76543210,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 9)),
                            (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 2)),
                            (TlvTag.LINK_ROLE, b"\x01"),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FF)),
                            (TlvTag.TRANSPORT, b"\x02"),
                            (TlvTag.AUTH_MODE, b"\x01"),
                            (TlvTag.AUTH_CHALLENGE, bytes.fromhex("44" * 32)),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        with pytest.raises(ProtocolError, match="requires a pairing token"):
            await task
        # Windows Python 3.8's loop clock may tick only every 15.6 ms.
        assert asyncio.get_running_loop().time() - started >= delay - resolution

    asyncio.run(scenario())


def test_unanswered_clock_probe_closes_stale_live_session() -> None:
    async def scenario() -> None:
        link = FakeLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link,
            on_ready,
            on_event,
            clock_sync_interval=0.01,
            clock_sync_timeout=0.01,
        )
        task = asyncio.create_task(session.run())
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 2),
                            ),
                            (TlvTag.LINK_ROLE, b"\x01"),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x0F)),
                            (TlvTag.TRANSPORT, b"\x01"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        await session.wait_ready()
        for _ in range(100):
            if link.closed:
                break
            await asyncio.sleep(0.002)
        assert link.closed is True
        probe_types = [
            decode_frame(wire[:-1]).type
            for wire in link.writes
            if decode_frame(wire[:-1]).channel == Channel.CONTROL
        ]
        assert probe_types.count(ControlType.TIME_SYNC_REQUEST) == 1
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_usb_serial_jtag_keeps_link_after_unanswered_clock_probes() -> None:
    from iris_gateway.console_protocol import decode_record, encode_record

    class ConsoleLink(FakeLink):
        console = True

        async def write(self, data):
            if data == b"iris hello\n":
                return
            frame = decode_record(data, request=True)
            self.writes.append(data)
            if frame.type == ControlType.HELLO_ACK:
                await self.incoming.put(encode_record(Frame(channel=Channel.CONTROL,
                    type=ControlType.AUTH_RESULT, session_id=frame.session_id,
                    payload=b"\x01"), request=False))

    async def scenario() -> None:
        link = ConsoleLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link,
            on_ready,
            on_event,
            clock_sync_interval=0.01,
            clock_sync_timeout=0.01,
        )
        task = asyncio.create_task(session.run())
        await link.incoming.put(
            encode_record(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 2),
                            ),
                            (TlvTag.LINK_ROLE, b"\x00"),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x0F)),
                            (TlvTag.TRANSPORT, b"\x03"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                ), request=False
            )
        )
        await session.wait_ready()
        for _ in range(100):
            probe_types = [
                decode_record(wire, request=True).type
                for wire in link.writes
                if decode_record(wire, request=True).channel == Channel.CONTROL
            ]
            if probe_types.count(ControlType.TIME_SYNC_REQUEST) >= 2:
                break
            await asyncio.sleep(0.002)
        assert probe_types.count(ControlType.TIME_SYNC_REQUEST) >= 2
        assert link.closed is False
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_rpc_jobs_screenshot_media_ota_and_restart() -> None:
    async def scenario() -> None:
        link = FakeLink()
        media_events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        async def on_media(event: dict[str, object]) -> None:
            media_events.append(event)

        session = DeviceSession(
            link, on_ready, on_event, on_media=on_media
        )
        task = asyncio.create_task(session.run())
        session_id = 0x10203040
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=session_id,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 12)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 2),
                            ),
                            (TlvTag.LINK_ROLE, b"\x01"),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FFF)),
                            (TlvTag.TRANSPORT, b"\x01"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        await session.wait_ready()
        assert session._clock_task is not None
        session._clock_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await session._clock_task
        session._clock_task = None

        control_sequence = 1
        rpc_task = asyncio.create_task(
            session.rpc(2, 3, b"request", deadline_ms=250)
        )
        rpc_index, rpc_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.REQUEST
        )
        assert struct.unpack_from("<HHIHH", rpc_request.payload) == (
            2,
            3,
            250,
            7,
            0,
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=rpc_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<HHiHH", 2, 3, 0, 5, 0)
                    + b"reply",
                )
            )
        )
        assert await rpc_task == b"reply"

        job_task = asyncio.create_task(session.job(77))
        _, job_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.JOB_QUERY, rpc_index + 1
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.JOB_STATUS,
                    flags=1,
                    session_id=session_id,
                    request_id=job_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<IHBBHHi", 77, 4, 1, 0, 500, 0, 0),
                )
            )
        )
        assert (await job_task)["progress_permille"] == 500

        screenshot_task = asyncio.create_task(
            session.screenshot({"width": 2, "height": 1, "format": 1})
        )
        screen_index, open_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.OPEN
        )
        description = struct.pack("<HHHHIHH", 0, 0, 2, 1, 4, 1, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.OPENED,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=open_request.request_id,
                    stream_id=55,
                    sequence=1,
                    payload=description + struct.pack("<I", 4),
                )
            )
        )
        read_index, read_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.READ, screen_index + 1
        )
        assert struct.unpack("<IHH", read_request.payload) == (0, 4, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.DATA,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=read_request.request_id,
                    stream_id=55,
                    sequence=2,
                    payload=struct.pack("<II", 0, 4) + b"shot" + struct.pack("<I", zlib.crc32(b"shot")),
                )
            )
        )
        _, close_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.CLOSE, read_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.CLOSE,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=close_request.request_id,
                    stream_id=55,
                    sequence=3,
                )
            )
        )
        actual, shot = await screenshot_task
        assert actual["width"] == 2
        assert shot == b"shot"

        mirror_task = asyncio.create_task(
            session.mirror_start(
                Channel.IMAGE, {"width": 1, "height": 1, "format": 3}
            )
        )
        mirror_index, mirror_request = await wait_for_request(
            link, Channel.IMAGE, MediaType.MIRROR_START
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.IMAGE,
                    type=MediaType.MIRROR_STATE,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=mirror_request.request_id,
                    stream_id=66,
                    sequence=1,
                    payload=mirror_request.payload,
                )
            )
        )
        assert (await mirror_task)["stream_id"] == 66
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.IMAGE,
                    type=MediaType.DATA,
                    session_id=session_id,
                    stream_id=66,
                    sequence=2,
                    payload=(
                        struct.pack("<QIIHH", 123, 9, 2, 0, 3)
                        + struct.pack("<HHHHIHH", 0, 0, 1, 1, 3, 3, 0)
                        + b"img"
                    ),
                )
            )
        )
        for _ in range(20):
            if media_events:
                break
            await asyncio.sleep(0)
        assert media_events[0]["data"] == b"img"

        credit_frames = [decode_frame(item[:-1]) for item in link.writes]
        assert any(item.type == ControlType.CREDIT and item.stream_id == 66
                   for item in credit_frames)
        stop_task = asyncio.create_task(session.mirror_stop(Channel.IMAGE))
        _, stop_request = await wait_for_request(link, Channel.IMAGE, MediaType.MIRROR_STOP)
        assert stop_request.stream_id == 66
        await link.incoming.put(encode_frame(Frame(
            channel=Channel.IMAGE, type=MediaType.MIRROR_STATE, flags=1 | 16,
            session_id=session_id, request_id=stop_request.request_id,
            stream_id=66, sequence=3,
        )))
        await stop_task

        image = b"abc"
        ota_progress = []

        async def record_ota_progress(item):
            ota_progress.append(item)

        ota_task = asyncio.create_task(
            session.ota_update(
                image,
                expected_sha256=hashlib.sha256(image).digest(),
                project_name="demo",
                version="1.0",
                progress_callback=record_ota_progress,
            )
        )
        ota_index, begin_request = await wait_for_request(
            link, Channel.OTA, OtaType.BEGIN
        )
        begin_response = struct.pack("<IIHB", 88, 3, 2, 4) + b"ota0"
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.BEGIN_RESPONSE,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=begin_request.request_id,
                    stream_id=88,
                    sequence=1,
                    payload=begin_response,
                )
            )
        )
        data_index, data_request = await wait_for_request(
            link, Channel.OTA, OtaType.DATA, ota_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.DATA_RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=data_request.request_id,
                    stream_id=88,
                    sequence=2,
                    payload=struct.pack("<IHH", 2, 600, 0),
                )
            )
        )
        data2_index, data2_request = await wait_for_request(
            link, Channel.OTA, OtaType.DATA, data_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.DATA_RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=data2_request.request_id,
                    stream_id=88,
                    sequence=3,
                    payload=struct.pack("<IHH", 3, 900, 0),
                )
            )
        )
        _, end_request = await wait_for_request(
            link, Channel.OTA, OtaType.END, data2_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.END_RESPONSE,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=end_request.request_id,
                    stream_id=88,
                    sequence=4,
                    payload=struct.pack("<Ii", 88, 0),
                )
            )
        )
        assert (await ota_task)["partition"] == "ota0"
        assert [item["stage"] for item in ota_progress] == [
            "erasing",
            "transferring",
            "transferring",
            "transferring",
            "verifying",
        ]
        assert [item["progress_permille"] for item in ota_progress] == [
            0,
            0,
            600,
            900,
            950,
        ]

        restart_task = asyncio.create_task(session.restart(300))
        _, restart_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.RESTART, mirror_index + 1
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.RESTART,
                    flags=1,
                    session_id=session_id,
                    request_id=restart_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<I", 300),
                )
            )
        )
        assert await restart_task == 300
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_update_waits_for_delayed_data_handshake_without_replaying_write() -> None:
    async def scenario() -> None:
        async def discard(value):
            pass
        control = DeviceSession(FakeLink(), discard, discard)
        data = DeviceSession(FakeLink(), discard, discard)
        control._console = True
        data._console = False
        data.owner_id = control.owner_id
        control.info = SimpleNamespace(device_id="device-1", boot_id=42)
        data.info = SimpleNamespace(device_id="device-1", boot_id=42)
        control._ready.set()
        submitted = []
        async def update(bundle, **kwargs):
            submitted.append(bundle)
            return {"accepted": True}
        data.system_update = update
        pending = asyncio.create_task(control.system_update("bundle", timeout=0.5))
        await asyncio.sleep(0.03)
        assert submitted == []
        control.data_session = data
        await asyncio.sleep(0.03)
        assert submitted == []
        data._ready.set()
        assert await pending == {"accepted": True}
        assert submitted == ["bundle"]
    asyncio.run(scenario())


@pytest.mark.parametrize("change", ["boot", "owner", "missing", "closed"])
def test_data_wait_rejects_changed_binding_or_times_out_before_write(change) -> None:
    async def scenario() -> None:
        async def discard(value):
            pass
        control = DeviceSession(FakeLink(), discard, discard)
        control.info = SimpleNamespace(device_id="device-1", boot_id=42)
        control._ready.set()
        pending = asyncio.create_task(control._wait_data_link(0.08))
        await asyncio.sleep(0.02)
        expected = ProtocolError
        if change == "boot":
            control.info.boot_id = 43
        elif change == "owner":
            control.owner_id = "another-owner"
        elif change == "closed":
            control._closed = True
            expected = ConnectionError
        else:
            expected = TimeoutError
        with pytest.raises(expected):
            await pending
    asyncio.run(scenario())
