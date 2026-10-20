from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import logging
import secrets
import struct
import time
import zlib
from typing import Any, Awaitable, Callable, Dict

from . import ota_transport, system_update_transport
from .console_input import CONSOLE_WRITE_TIMEOUT_SECONDS, encode_console_line
from .console_protocol import HELLO_COMMAND, ConsoleDecoder, encode_record
from .device_info import DeviceInfo
from .files import DeviceFiles
from .link import Link
from .protocol import (
    VERSION,
    Capability,
    Channel,
    ControlType,
    CrashType,
    EventType,
    Frame,
    FrameDecoder,
    JobState,
    MediaType,
    ProtocolError,
    TlvTag,
    decode_tlv,
    encode_frame,
    tlv_u8,
    tlv_u16,
    tlv_u32,
    tlv_u64,
)
from .state_machine import SessionEvent, SessionState, session_transition
from .system_update import SystemUpdateBundle

EventCallback = Callable[[Dict[str, Any]], Awaitable[None]]
ProgressCallback = Callable[[Dict[str, Any]], Awaitable[None]]
MediaCallback = Callable[[Dict[str, Any]], Awaitable[None]]
ReadyCallback = Callable[["DeviceSession"], Awaitable[None]]
_LOGGER = logging.getLogger(__name__)


async def _discard_media(event: dict[str, Any]) -> None:
    del event


class DeviceError(RuntimeError):
    def __init__(self, code: int):
        self.code = code
        super().__init__(f"device error 0x{code:08x}")


class DeviceSession:
    LOG_CREDIT_GRANT = 256 * 1024
    LOG_CREDIT_LOW_WATER = 128 * 1024
    AUTH_MISSING_TOKEN_DELAY_SECONDS = 0.5
    CONSOLE_PROBE_INTERVAL_SECONDS = 2.0
    # The bounded firmware service executor admits one Flash request at a
    # time. No larger receive window is negotiated by protocol 0.2.
    OTA_MAX_IN_FLIGHT = 1
    OTA_BEGIN_TIMEOUT_SECONDS = 120.0

    def __init__(
        self,
        link: Link,
        on_ready: ReadyCallback,
        on_event: EventCallback,
        *,
        on_media: MediaCallback | None = None,
        pairing_token: str | bytes | None = None,
        owner_id: bytes | None = None,
        clock_sync_interval: float = 30.0,
        clock_sync_timeout: float = 3.0,
    ) -> None:
        if clock_sync_interval <= 0 or clock_sync_timeout <= 0:
            raise ValueError("clock sync interval and timeout must be positive")
        self.link = link
        self.info: DeviceInfo | None = None
        self._on_ready = on_ready
        self._on_event = on_event
        self._on_media = on_media or _discard_media
        if isinstance(pairing_token, str):
            try:
                pairing_token = bytes.fromhex(pairing_token)
            except ValueError as exc:
                raise ValueError("pairing token must be 64 hex characters") from exc
        if pairing_token is not None and len(pairing_token) != 32:
            raise ValueError("pairing token must contain 32 bytes")
        self._pairing_token = pairing_token
        self.owner_id = owner_id if owner_id is not None else secrets.token_bytes(16)
        if len(self.owner_id) != 16 or not any(self.owner_id):
            raise ValueError("link owner ID must contain 16 nonzero random bytes")
        self.data_session: DeviceSession | None = None
        self.data_tcp_port: int | None = None
        self.data_available = False
        self._clock_sync_interval = clock_sync_interval
        self._clock_sync_timeout = clock_sync_timeout
        self._console = getattr(link, "console", False) is True
        self._decoder = ConsoleDecoder() if self._console else FrameDecoder()
        self.capture_id = secrets.token_hex(16) if self._console else None
        self._capture_offset = 0
        self._hello_task: asyncio.Task[None] | None = None
        self.state = SessionState.NEGOTIATING
        self._write_lock = asyncio.Lock()
        self._request_lock = asyncio.Lock()
        self._pending: dict[int, asyncio.Future[Frame]] = {}
        self._request_id = 0
        channel_count = max(int(channel) for channel in Channel) + 1
        self._reopen_session_id: int | None = None
        self._reopen_requested = False
        self._sequence = [0] * channel_count
        self._last_rx_sequence: list[int | None] = [None] * channel_count
        self._closed = False
        self._ready = asyncio.Event()
        self._clock_task: asyncio.Task[None] | None = None
        self._log_credit = 0
        self._crash_chunk_max = 1024
        self._ready_announced = False
        self._media_credit = [0] * channel_count
        self._media_streams: dict[int, int] = {}
        self._credit_tasks: dict[int, asyncio.Task[None]] = {}
        self._files = DeviceFiles(self)
        self.clock_offset_us: float | None = None
        self.clock_uncertainty_us: float | None = None

    @property
    def files(self) -> DeviceFiles:
        # Selecting the service pins the transfer to one concrete data session.
        # A reconnect must not silently move an active transfer to another link.
        return self._require_data_link()._files if self._console else self._files

    @property
    def console_available(self) -> bool:
        return self._console and not self._closed and self.info is not None and self.state is SessionState.READY

    async def console_write(self, line: str) -> dict[str, Any]:
        wire = encode_console_line(line)
        async with self._write_lock:
            if not self.console_available:
                raise ConnectionError("a live console control link is required")
            try:
                await asyncio.wait_for(self.link.write(wire), CONSOLE_WRITE_TIMEOUT_SECONDS)
            except (Exception, asyncio.CancelledError):
                # A partial line must never be followed by a machine record or
                # retried on a replacement session: its execution is unknown.
                await self.close()
                raise
            return {"sent": True, "bytes_written": len(wire), "endpoint": self.link.endpoint,
                    "completion": "unconfirmed"}

    async def run(self) -> None:
        try:
            if self._console:
                self._hello_task = asyncio.create_task(self._console_probe_loop())
            while not self._closed:
                data = await self.link.read()
                if not data:
                    raise ConnectionError(f"ESP-Iris link closed: {self.link.endpoint}")
                if self._console:
                    await self._on_event({
                        "kind": "console_raw", "endpoint": self.link.endpoint,
                        "capture_id": self.capture_id, "offset": self._capture_offset,
                        "device_id": self.info.device_id if self.info else None,
                        "boot_id": None,
                        "host_receive_wall_ns": time.time_ns(),
                        "data_base64": base64.b64encode(data).decode("ascii"),
                    })
                    self._capture_offset += len(data)
                for frame in self._decoder.feed(data):
                    if isinstance(frame, bytes):
                        await self._console_log(frame)
                    else:
                        await self._handle_frame(frame, time.monotonic_ns())
        finally:
            if self._hello_task is not None:
                self._hello_task.cancel()
                await asyncio.gather(self._hello_task, return_exceptions=True)
            if isinstance(self._decoder, ConsoleDecoder):
                remainder = self._decoder.finish()
                if remainder:
                    await self._console_log(remainder)
            self._closed = True
            if self.state is not SessionState.CLOSED:
                self.state = session_transition(self.state, SessionEvent.CLOSE)
            self._fail_pending()
            try:
                await self._cancel_credit_tasks()
                if self._clock_task is not None:
                    self._clock_task.cancel()
                    # Consume child cancellation, not run() cancellation during
                    # EOF cleanup: otherwise the supervisor can retry forever.
                    await asyncio.gather(self._clock_task, return_exceptions=True)
            finally:
                await self.link.close()

    async def _console_probe_loop(self) -> None:
        # The read loop starts before discovery writes. Keep capturing even
        # when the peer runs a bootloader, a third-party app, or a slow startup.
        await asyncio.sleep(0)
        while not self._closed:
            try:
                async with self._write_lock:
                    if self._closed:
                        return
                    await self.link.write(HELLO_COMMAND)
            except (ConnectionError, OSError):
                await self.link.close()
                return
            # UART and Serial/JTAG may reboot without closing the port. Keep
            # querying identity so the supervisor can replace a stale session.
            await asyncio.sleep(self.CONSOLE_PROBE_INTERVAL_SECONDS)

    async def _console_log(self, data: bytes) -> None:
        await self._on_event({
            "kind": "log", "source": "console", "endpoint": self.link.endpoint,
            "capture_id": self.capture_id,
            "device_id": self.info.device_id if self.info else None,
            "boot_id": None,
            "monotonic_us": None, "host_receive_wall_ns": time.time_ns(),
            "text": data.decode("utf-8", errors="replace"),
        })

    async def close(self) -> None:
        self._closed = True
        if self.state is not SessionState.CLOSED:
            self.state = session_transition(self.state, SessionEvent.CLOSE)
        self._fail_pending()
        await self._cancel_credit_tasks()
        await self.link.close()

    async def _cancel_credit_tasks(self) -> None:
        current = asyncio.current_task()
        entries = [(channel, task) for channel, task in self._credit_tasks.items()
                   if task is not current]
        tasks = [task for _, task in entries]
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        # Cancellation before a coroutine's first turn never enters its
        # finally block. Reclaim those dictionary entries explicitly too.
        for channel, task in entries:
            if self._credit_tasks.get(channel) is task:
                self._credit_tasks.pop(channel, None)

    def _queue_credit(self, channel: int, amount: int) -> None:
        """Never block the receive loop on a duplex USB write.

        A large RPC can occupy the writer while the device drains TX before
        reading RX. Waiting for that writer here stops host RX and deadlocks
        both sides with small CDC FIFOs. Coalesce to one task per channel.
        """
        channel = int(channel)
        if self._closed or channel in self._credit_tasks:
            return

        async def grant() -> None:
            try:
                if channel == int(Channel.LOG):
                    await self._grant_log_credit(amount)
                else:
                    await self._grant_media_credit(channel, amount)
            except asyncio.CancelledError:
                raise
            except Exception:
                # Wake the supervisor/pending requests on a failed write;
                # do not leave an unobserved background task exception.
                _LOGGER.exception("ESP-Iris credit replenishment failed on %s", self.link.endpoint)
                await self.close()
            finally:
                self._credit_tasks.pop(channel, None)

        self._credit_tasks[channel] = asyncio.create_task(grant())

    def _fail_pending(self) -> None:
        for future in self._pending.values():
            if not future.done():
                future.set_exception(ConnectionError("ESP-Iris session closed"))
        self._pending.clear()

    async def wait_ready(self, timeout: float = 5.0) -> DeviceInfo:
        await asyncio.wait_for(self._ready.wait(), timeout)
        assert self.info is not None
        return self.info

    def _next_request_id(self) -> int:
        self._request_id = (self._request_id + 1) & 0xFFFFFFFF
        if self._request_id == 0:
            self._request_id = 1
        return self._request_id

    async def _send(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        *,
        flags: int = 0,
        request_id: int = 0,
        stream_id: int = 0,
    ) -> None:
        if self.info is None:
            session_id = 0
        else:
            session_id = self.info.session_id
        async with self._write_lock:
            self._sequence[int(channel)] = (self._sequence[int(channel)] + 1) & 0xFFFFFFFF
            frame = Frame(
                channel=channel,
                type=type_,
                flags=flags,
                session_id=session_id,
                request_id=request_id,
                stream_id=stream_id,
                sequence=self._sequence[int(channel)],
                payload=payload,
            )
            await self.link.write(encode_record(frame) if self._console else encode_frame(frame))

    async def _request_unlocked(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        timeout: float = 3.0,
        *,
        stream_id: int = 0,
    ) -> Frame:
        await self.wait_ready(timeout)
        if self._closed:
            raise ConnectionError("ESP-Iris session closed")
        request_id = self._next_request_id()
        future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        try:
            await self._send(
                channel,
                type_,
                payload,
                request_id=request_id,
                stream_id=stream_id,
            )
            return await asyncio.wait_for(future, timeout)
        finally:
            self._pending.pop(request_id, None)
            if future.done() and not future.cancelled():
                # close() can fail a pending request while its write is still
                # unwinding; consume that error even if write() also failed.
                future.exception()

    @staticmethod
    def _uses_data_link(channel: int, type_: int) -> bool:
        if channel in (Channel.FILE, Channel.OTA, Channel.SYSTEM_UPDATE):
            # Inventory is a bounded read-only control request.
            return not (channel == Channel.SYSTEM_UPDATE and type_ == 0x0e)
        return channel in (Channel.IMAGE, Channel.AUDIO) or (
            channel == Channel.SCREEN and type_ in (MediaType.MIRROR_START, MediaType.MIRROR_STOP))

    def _require_data_link(self) -> DeviceSession:
        data = self.data_session
        if data is None or data.info is None or self.info is None or data._closed or not data._ready.is_set():
            raise RuntimeError("operation requires an independently connected Iris 0.2 data link")
        if data.info.device_id != self.info.device_id or data.info.boot_id != self.info.boot_id or data.owner_id != self.owner_id:
            raise ProtocolError("data link identity/session binding changed")
        return data

    async def _wait_data_link(self, timeout: float) -> DeviceSession:
        """Wait for this boot's independent data handshake before a write."""
        info = await self.wait_ready(timeout)
        identity = (info.device_id, info.boot_id, self.owner_id)
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            if self._closed:
                raise ConnectionError("ESP-Iris control session closed before data handshake")
            if self.info is None or (
                self.info.device_id, self.info.boot_id, self.owner_id
            ) != identity:
                raise ProtocolError("control identity changed before data handshake")
            data = self.data_session
            if data is not None and data.info is not None and data._ready.is_set():
                if data._closed:
                    data = None
                else:
                    return self._require_data_link()
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise TimeoutError("independent Iris data link did not become ready")
            await asyncio.sleep(min(0.02, remaining))

    async def _request(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        timeout: float = 3.0,
        *,
        stream_id: int = 0,
    ) -> Frame:
        # These CONTROL probes cannot mutate firmware. They must remain
        # observable while a slow RPC owns the mutation lock. Request IDs
        # correlate concurrent replies and _send serializes sequence/wire order.
        if channel == Channel.CONTROL and type_ in (
            ControlType.STATUS_REQUEST, ControlType.PING, ControlType.TIME_SYNC_REQUEST
        ):
            return await self._request_unlocked(
                channel, type_, payload, timeout, stream_id=stream_id
            )
        if self._console and self._uses_data_link(channel, type_):
            data = self._require_data_link()
            return await data._request(channel, type_, payload, timeout, stream_id=stream_id)
        async with self._request_lock:
            return await self._request_unlocked(
                channel,
                type_,
                payload,
                timeout,
                stream_id=stream_id,
            )

    @staticmethod
    def _text(fields: dict[int, bytes], tag: TlvTag) -> str:
        return fields.get(int(tag), b"").decode("utf-8", errors="replace")

    async def _handle_hello(self, frame: Frame) -> None:
        fields = decode_tlv(frame.payload)
        raw_device_id = fields.get(int(TlvTag.DEVICE_ID), b"")
        if len(raw_device_id) != 16:
            raise ProtocolError("HELLO is missing a 16-byte device ID")
        raw_hardware_mac = fields.get(int(TlvTag.HARDWARE_MAC), b"")
        if raw_hardware_mac and len(raw_hardware_mac) != 6:
            raise ProtocolError("HELLO has an invalid hardware MAC")
        if raw_hardware_mac:
            expected_device_id = b"ESP-IRIS\x01\x00" + raw_hardware_mac
            if raw_device_id != expected_device_id:
                raise ProtocolError(
                    "device ID does not match the factory hardware MAC"
                )
        protocol_version = tlv_u16(fields, TlvTag.PROTOCOL_VERSION)
        if protocol_version != VERSION:
            raise ProtocolError(f"unsupported ESP-Iris protocol {protocol_version}")
        for tag, size in ((TlvTag.FIRMWARE_ROLE, 1), (TlvTag.RECOVERY_ABI, 2),
                          (TlvTag.REQUIRED_FEATURES, 8), (TlvTag.HEALTH_TIMEOUT_MS, 4)):
            if tag in fields and len(fields[tag]) != size:
                raise ProtocolError(f"invalid {tag.name} length")
        for tag in (TlvTag.PRODUCT_CONTRACT, TlvTag.CHIP_TARGET,
                    TlvTag.BOARD_ID, TlvTag.LAYOUT_ID):
            if len(fields.get(tag, b"")) > 64:
                raise ProtocolError(f"{tag.name} exceeds 64 bytes")
        required_features = tlv_u64(fields, TlvTag.REQUIRED_FEATURES)
        if required_features:
            raise ProtocolError(f"unsupported required features 0x{required_features:x}")
        role = tlv_u8(fields, TlvTag.FIRMWARE_ROLE)
        if role not in (0, 1, 2):
            raise ProtocolError(f"unsupported firmware role {role}")
        health_timeout_ms = tlv_u32(fields, TlvTag.HEALTH_TIMEOUT_MS, 45000)
        if not 1000 <= health_timeout_ms <= 600000:
            raise ProtocolError("health timeout must be between 1000 and 600000 ms")

        expected_role = b"\x00" if self._console else b"\x01"
        if fields.get(TlvTag.LINK_ROLE) != expected_role:
            raise ProtocolError("HELLO link role does not match the opened endpoint")
        self.data_available = tlv_u8(fields, TlvTag.DATA_AVAILABLE) == 1
        self.data_tcp_port = tlv_u16(fields, TlvTag.DATA_TCP_PORT) or None
        info = DeviceInfo(
            device_id=raw_device_id.hex(),
            boot_id=tlv_u64(fields, TlvTag.BOOT_ID),
            session_id=frame.session_id,
            endpoint=self.link.endpoint,
            transport=tlv_u8(fields, TlvTag.TRANSPORT),
            project_name=self._text(fields, TlvTag.PROJECT_NAME),
            app_version=self._text(fields, TlvTag.APP_VERSION),
            idf_version=self._text(fields, TlvTag.IDF_VERSION),
            firmware_sha256=fields.get(int(TlvTag.FIRMWARE_SHA256), b"").hex(),
            reset_reason=tlv_u32(fields, TlvTag.RESET_REASON),
            capabilities=tlv_u64(fields, TlvTag.CAPABILITIES),
            auth_mode=tlv_u8(fields, TlvTag.AUTH_MODE),
            link_role="control" if self._console else "data",
            max_payload=tlv_u32(fields, TlvTag.MAX_PAYLOAD, 4000),
            firmware_mode={0: "unknown", 1: "normal", 2: "recovery"}[role],
            product_contract=self._text(fields, TlvTag.PRODUCT_CONTRACT),
            chip_target=self._text(fields, TlvTag.CHIP_TARGET),
            board_id=self._text(fields, TlvTag.BOARD_ID),
            layout_id=self._text(fields, TlvTag.LAYOUT_ID),
            recovery_abi=tlv_u16(fields, TlvTag.RECOVERY_ABI),
            required_features=required_features,
            health_timeout_ms=health_timeout_ms,
            hardware_mac=":".join(f"{byte:02x}" for byte in raw_hardware_mac),
        )
        reopening = (
            self._reopen_session_id is not None
            and info.session_id != self._reopen_session_id
        )
        if reopening:
            if (
                self.info is None
                or self.info.device_id != info.device_id
                or self.info.boot_id != info.boot_id
            ):
                raise ProtocolError("device identity changed during session reopen")
            self._reopen_session_id = None
            self._sequence = [0] * len(self._sequence)
            self._last_rx_sequence = [None] * len(self._last_rx_sequence)
        if self.info is not None and not reopening and (
            self.info.device_id != info.device_id
            or self.info.session_id != info.session_id
            or self.info.boot_id != info.boot_id
        ):
            raise ProtocolError("device identity/session changed on a live link")
        self.info = info
        if self._console and self._ready.is_set():
            # An unchanged periodic HELLO is not a new authentication attempt.
            return
        if (
            info.capabilities & Capability.SESSION_REOPEN
            and not self._reopen_requested
        ):
            self._reopen_requested = True
            self._reopen_session_id = info.session_id
        ack_flags = 1 << 5 if self._reopen_session_id is not None else 0
        binding = expected_role + self.owner_id
        if info.auth_mode == 0:
            await self._send(Channel.CONTROL, ControlType.HELLO_ACK, binding, flags=ack_flags)
            return
        if info.auth_mode != 1:
            raise ProtocolError(f"unsupported ESP-Iris auth mode {info.auth_mode}")
        challenge = fields.get(int(TlvTag.AUTH_CHALLENGE), b"")
        if len(challenge) != 32:
            raise ProtocolError("authenticated HELLO has no 32-byte challenge")
        if self._pairing_token is None:
            await asyncio.sleep(self.AUTH_MISSING_TOKEN_DELAY_SECONDS)
            raise ProtocolError("device requires a pairing token")
        nonce = secrets.token_bytes(16)
        message = (
            b"ESP-Iris-auth-0.2"
            + raw_device_id
            + struct.pack("<QI", info.boot_id, info.session_id)
            + challenge
            + nonce
            + binding
        )
        proof = hmac.new(self._pairing_token, message, hashlib.sha256).digest()
        await self._send(
            Channel.CONTROL, ControlType.HELLO_ACK, binding + nonce + proof, flags=ack_flags
        )

    async def _complete_ready(self) -> None:
        if self._ready_announced:
            return
        assert self.info is not None
        self.state = session_transition(
            self.state, SessionEvent.AUTHENTICATED
        )
        if self._log_credit == 0 and not self._console:
            await self._grant_log_credit(self.LOG_CREDIT_GRANT)
        await self._on_ready(self)
        if self._console:
            await self._on_event({
                "kind": "console_binding", "capture_id": self.capture_id,
                "endpoint": self.link.endpoint, "device_id": self.info.device_id,
                "offset": self._capture_offset,
                "boot_id": self.info.boot_id, "host_receive_wall_ns": time.time_ns(),
            })
        self._ready_announced = True
        self._ready.set()
        self._clock_task = asyncio.create_task(
            self._clock_loop(), name=f"iris-clock-{self.info.device_id}"
        )

    async def _grant_log_credit(self, amount: int) -> None:
        payload = bytes([int(Channel.LOG), 0, 0, 0]) + struct.pack("<I", amount)
        await self._send(Channel.CONTROL, ControlType.CREDIT, payload)
        self._log_credit += amount

    async def _handle_log(self, frame: Frame) -> None:
        if len(frame.payload) < 16:
            raise ProtocolError("short LOG record")
        monotonic_us, dropped, source, flags, length = struct.unpack_from(
            "<QIBBH", frame.payload
        )
        if len(frame.payload) != 16 + length:
            raise ProtocolError("LOG record length mismatch")
        data = frame.payload[16:]
        self._log_credit = max(0, self._log_credit - len(frame.payload))
        event: dict[str, Any] = {
            "kind": "log",
            "device_id": self.info.device_id if self.info else None,
            "boot_id": self.info.boot_id if self.info else None,
            "monotonic_us": monotonic_us,
            "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
            "dropped_bytes": dropped,
            "source": "stderr" if source == 2 else "stdout",
            "flags": flags,
            "text": data.decode("utf-8", errors="replace"),
        }
        await self._on_event(event)
        if self._log_credit < self.LOG_CREDIT_LOW_WATER:
            self._queue_credit(Channel.LOG, self.LOG_CREDIT_GRANT)

    async def _handle_event(self, frame: Frame, host_receive_ns: int) -> None:
        if frame.type == EventType.JOB_UPDATE:
            job = self._decode_job(frame.payload)
            await self._on_event(
                {
                    "kind": "job",
                    "device_id": self.info.device_id if self.info else None,
                    "boot_id": self.info.boot_id if self.info else None,
                    "session_id": self.info.session_id if self.info else None,
                    "host_receive_monotonic_ns": host_receive_ns,
                    "host_receive_wall_ns": time.time_ns(),
                    **job,
                }
            )
            return
        fields = decode_tlv(frame.payload)
        monotonic_us = tlv_u64(fields, TlvTag.UPTIME_US)
        boot_id = tlv_u64(
            fields, TlvTag.BOOT_ID, self.info.boot_id if self.info else 0
        )
        await self._on_event(
            {
                "kind": "device_event",
                "event_type": frame.type,
                "event_name": EventType(frame.type).name.lower()
                if frame.type in EventType._value2member_map_
                else "unknown",
                "device_id": self.info.device_id if self.info else None,
                "boot_id": boot_id,
                "session_id": self.info.session_id if self.info else None,
                "monotonic_us": monotonic_us,
                "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
                "host_receive_monotonic_ns": host_receive_ns,
                "host_receive_wall_ns": time.time_ns(),
                "clock_uncertainty_us": self.clock_uncertainty_us,
                "reset_reason": tlv_u32(fields, TlvTag.RESET_REASON),
                "crash_count": tlv_u32(fields, TlvTag.CRASH_COUNT),
                "crash_limit": tlv_u32(fields, TlvTag.CRASH_LIMIT),
                "crash_recovery_pending": bool(
                    tlv_u8(fields, TlvTag.CRASH_RECOVERY_PENDING)
                ),
                "crash_origin_reset_reason": tlv_u32(
                    fields, TlvTag.CRASH_ORIGIN_RESET_REASON
                ),
                "crash_failed_app_address": tlv_u32(
                    fields, TlvTag.CRASH_FAILED_APP_ADDRESS
                ),
                "crash_failed_boot_id": tlv_u64(
                    fields, TlvTag.CRASH_FAILED_BOOT_ID
                ),
                "crash_failed_firmware_sha256": fields.get(
                    int(TlvTag.CRASH_FAILED_FIRMWARE_SHA256), b""
                ).hex(),
                "crash_state_error": tlv_u32(
                    fields, TlvTag.CRASH_STATE_ERROR
                ),
                "sequence": frame.sequence,
                "event_id": (
                    f"{self.info.device_id if self.info else 'unknown'}:"
                    f"{boot_id}:{monotonic_us}:{frame.sequence}"
                ),
            }
        )

    @staticmethod
    def _decode_job(payload: bytes) -> dict[str, Any]:
        if len(payload) != 16:
            raise ProtocolError("invalid job status size")
        job_id, kind, state, cancelled, progress, reserved, result = (
            struct.unpack("<IHBBHHi", payload)
        )
        if reserved != 0:
            raise ProtocolError("job status reserved field is nonzero")
        return {
            "job_id": job_id,
            "job_kind": kind,
            "job_state": JobState(state).name.lower()
            if state in JobState._value2member_map_
            else "unknown",
            "job_state_code": state,
            "cancel_requested": bool(cancelled),
            "progress_permille": progress,
            "result": result,
        }

    async def _grant_media_credit(self, channel: int, amount: int) -> None:
        if self._console:
            await self._require_data_link()._grant_media_credit(channel, amount)
            return
        payload = bytes([int(channel), 0, 0, 0]) + struct.pack("<I", amount)
        await self._send(Channel.CONTROL, ControlType.CREDIT, payload,
                         stream_id=self._media_streams.get(int(channel), 0))
        self._media_credit[int(channel)] += amount

    async def _handle_media(self, frame: Frame) -> None:
        if frame.type != MediaType.DATA or len(frame.payload) < 36:
            raise ProtocolError("invalid media data frame")
        (
            monotonic_us,
            frame_id,
            dropped,
            flags,
            data_size,
            x,
            y,
            width,
            height,
            stride,
            format_,
            quality,
        ) = struct.unpack_from("<QIIHHHHHHIHH", frame.payload)
        if len(frame.payload) != 36 + data_size:
            raise ProtocolError("media data size mismatch")
        self._media_credit[int(frame.channel)] = max(
            0, self._media_credit[int(frame.channel)] - len(frame.payload)
        )
        await self._on_media(
            {
                "kind": "media",
                "device_id": self.info.device_id if self.info else None,
                "boot_id": self.info.boot_id if self.info else None,
                "session_id": self.info.session_id if self.info else None,
                "channel": int(frame.channel),
                "stream_id": frame.stream_id,
                "frame_id": frame_id,
                "monotonic_us": monotonic_us,
                "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
                "dropped": dropped,
                "flags": flags,
                "description": {
                    "x": x,
                    "y": y,
                    "width": width,
                    "height": height,
                    "stride": stride,
                    "format": format_,
                    "quality": quality,
                },
                "data": frame.payload[36:],
            }
        )
        if self._media_credit[int(frame.channel)] < 64 * 1024:
            self._queue_credit(frame.channel, 128 * 1024)

    def _accept_sequence(self, frame: Frame) -> bool:
        channel = int(frame.channel)
        last = self._last_rx_sequence[channel]
        if last is None:
            self._last_rx_sequence[channel] = frame.sequence
            return True
        distance = (frame.sequence - last) & 0xFFFFFFFF
        if distance == 0 or distance >= 0x80000000:
            return False
        self._last_rx_sequence[channel] = frame.sequence
        return True

    async def _handle_frame(self, frame: Frame, host_receive_ns: int) -> None:
        if frame.channel == Channel.CONTROL and frame.type == ControlType.HELLO:
            await self._handle_hello(frame)
            return
        if self._reopen_session_id is not None:
            return
        if self.info is None or frame.session_id != self.info.session_id:
            return
        if not self._accept_sequence(frame):
            return
        if (
            frame.channel == Channel.CONTROL
            and frame.type == ControlType.AUTH_RESULT
        ):
            if frame.payload != b"\x01" or frame.flags & 0x02:
                raise ProtocolError("ESP-Iris pairing proof was rejected")
            await self._complete_ready()
            return
        if frame.request_id and frame.request_id in self._pending:
            future = self._pending[frame.request_id]
            if not future.done():
                if frame.type == ControlType.ERROR:
                    code = struct.unpack_from("<I", frame.payload + b"\0\0\0\0")[0]
                    future.set_exception(DeviceError(code))
                else:
                    future.set_result(frame)
            return
        if frame.channel == Channel.LOG and frame.type == 1:
            await self._handle_log(frame)
        elif frame.channel == Channel.EVENT:
            await self._handle_event(frame, host_receive_ns)
        elif frame.channel in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            await self._handle_media(frame)

    async def status(self) -> dict[str, Any]:
        identity = (self.info.device_id, self.info.boot_id, self.info.session_id) if self.info else None
        try:
            frame = await self._request(Channel.CONTROL, ControlType.STATUS_REQUEST, timeout=5.0)
        except asyncio.TimeoutError:
            # STATUS is read-only. One new correlated request tolerates a
            # delayed reply during bulk traffic without replaying any write or
            # carrying a probe across a changed session/boot.
            current = (self.info.device_id, self.info.boot_id, self.info.session_id) if self.info else None
            if self._closed or current != identity:
                raise
            frame = await self._request(Channel.CONTROL, ControlType.STATUS_REQUEST, timeout=5.0)
        if frame.type != ControlType.STATUS_RESPONSE:
            raise ProtocolError("unexpected status response")
        fields = decode_tlv(frame.payload)
        assert self.info is not None
        raw_hardware_mac = fields.get(int(TlvTag.HARDWARE_MAC), b"")
        if raw_hardware_mac:
            hardware_mac = ":".join(f"{byte:02x}" for byte in raw_hardware_mac)
            if hardware_mac != self.info.hardware_mac:
                raise ProtocolError("hardware MAC changed on a live session")
        return {
            **self.info.as_dict(),
            "uptime_us": tlv_u64(fields, TlvTag.UPTIME_US),
            "free_internal": tlv_u32(fields, TlvTag.FREE_INTERNAL),
            "min_free_internal": tlv_u32(fields, TlvTag.MIN_FREE_INTERNAL),
            "total_internal": tlv_u32(fields, TlvTag.TOTAL_INTERNAL),
            "total_spiram": tlv_u32(fields, TlvTag.TOTAL_SPIRAM),
            "free_spiram": tlv_u32(fields, TlvTag.FREE_SPIRAM),
            "min_free_spiram": tlv_u32(fields, TlvTag.MIN_FREE_SPIRAM),
            "log_dropped_bytes": tlv_u32(fields, TlvTag.LOG_DROPPED),
            "rx_frames": tlv_u32(fields, TlvTag.RX_FRAMES),
            "tx_frames": tlv_u32(fields, TlvTag.TX_FRAMES),
            "invalid_frames": tlv_u32(fields, TlvTag.INVALID_FRAMES),
            "link_count": tlv_u32(fields, TlvTag.LINK_COUNT),
            "task_stack_free_min_bytes": tlv_u32(
                fields, TlvTag.TASK_STACK_FREE_MIN
            ),
            "worker_active_max_us": tlv_u32(
                fields, TlvTag.WORKER_ACTIVE_MAX_US
            ),
            "lifecycle_state": tlv_u8(fields, TlvTag.LIFECYCLE_STATE),
            "internal_heap_used_bytes": tlv_u32(
                fields, TlvTag.INTERNAL_HEAP_USED
            ),
            "static_internal_bytes": tlv_u32(
                fields, TlvTag.STATIC_INTERNAL_BYTES
            ),
            "internal_total_bytes": (
                tlv_u32(fields, TlvTag.STATIC_INTERNAL_BYTES)
                + tlv_u32(fields, TlvTag.INTERNAL_HEAP_USED)
            ),
            "crash_count": tlv_u32(fields, TlvTag.CRASH_COUNT),
            "crash_limit": tlv_u32(fields, TlvTag.CRASH_LIMIT),
            "crash_loop_triggered": bool(
                tlv_u8(fields, TlvTag.CRASH_LOOP_TRIGGERED)
            ),
            "crash_recovery_pending": bool(
                tlv_u8(fields, TlvTag.CRASH_RECOVERY_PENDING)
            ),
            "crash_origin_reset_reason": tlv_u32(
                fields, TlvTag.CRASH_ORIGIN_RESET_REASON
            ),
            "crash_failed_app_address": tlv_u32(
                fields, TlvTag.CRASH_FAILED_APP_ADDRESS
            ),
            "crash_failed_boot_id": tlv_u64(
                fields, TlvTag.CRASH_FAILED_BOOT_ID
            ),
            "crash_failed_firmware_sha256": fields.get(
                int(TlvTag.CRASH_FAILED_FIRMWARE_SHA256), b""
            ).hex(),
            "crash_state_error": tlv_u32(fields, TlvTag.CRASH_STATE_ERROR),
            "clock_offset_us": self.clock_offset_us,
            "clock_uncertainty_us": self.clock_uncertainty_us,
        }

    async def task_memory(self) -> dict[str, Any]:
        if self.info is None or not self.info.capabilities & Capability.TASK_MEMORY:
            raise NotImplementedError("task memory observation is unavailable")
        frame = await self._request(Channel.CONTROL, ControlType.TASKS_REQUEST)
        if frame.type != ControlType.TASKS_RESPONSE or len(frame.payload) < 12:
            raise ProtocolError("unexpected task memory response")
        uptime_us, count, reserved = struct.unpack_from("<QHH", frame.payload)
        if reserved or len(frame.payload) != 12 + count * 8:
            raise ProtocolError("invalid task memory response")
        tasks = [
            {"task_number": number, "stack_free_min_bytes": free_bytes}
            for number, free_bytes in struct.iter_unpack("<II", frame.payload[12:])
        ]
        return {
            **self.info.as_dict(),
            "uptime_us": uptime_us,
            "tasks": tasks,
        }

    async def rpc(
        self,
        service_id: int,
        method_id: int,
        payload: bytes = b"",
        *,
        deadline_ms: int = 1000,
        timeout: float = 3.0,
    ) -> bytes:
        if (
            not 1 <= service_id <= 0xFFFF
            or not 1 <= method_id <= 0xFFFF
            or not 0 <= deadline_ms <= 0xFFFFFFFF
            or len(payload) > 1024
        ):
            raise ValueError("invalid RPC request")
        request = struct.pack(
            "<HHIHH", service_id, method_id, deadline_ms, len(payload), 0
        ) + payload
        frame = await self._request(
            Channel.CONTROL, ControlType.REQUEST, request, timeout
        )
        if frame.type != ControlType.RESPONSE or len(frame.payload) < 12:
            raise ProtocolError("unexpected RPC response")
        returned_service, returned_method, result, size, reserved = (
            struct.unpack_from("<HHiHH", frame.payload)
        )
        if (
            returned_service != service_id
            or returned_method != method_id
            or reserved != 0
            or len(frame.payload) != 12 + size
        ):
            raise ProtocolError("invalid RPC response")
        if result != 0:
            raise RuntimeError(f"RPC failed with device error 0x{result:08x}")
        return frame.payload[12:]

    async def job(self, job_id: int, *, cancel: bool = False) -> dict[str, Any]:
        if not 1 <= job_id <= 0xFFFFFFFF:
            raise ValueError("invalid job ID")
        frame = await self._request(
            Channel.CONTROL,
            ControlType.CANCEL if cancel else ControlType.JOB_QUERY,
            struct.pack("<I", job_id),
        )
        if frame.type != ControlType.JOB_STATUS:
            raise ProtocolError("unexpected job response")
        return self._decode_job(frame.payload)

    @staticmethod
    def _encode_media_description(description: dict[str, int]) -> bytes:
        values = (
            description.get("x", 0),
            description.get("y", 0),
            description.get("width", 0),
            description.get("height", 0),
            description.get("stride", 0),
            description.get("format", 1),
            description.get("quality", 0),
        )
        if any(value < 0 for value in values):
            raise ValueError("media description fields must be nonnegative")
        return struct.pack("<HHHHIHH", *values)

    @staticmethod
    def _decode_media_description(payload: bytes) -> dict[str, int]:
        if len(payload) < 16:
            raise ProtocolError("short media description")
        x, y, width, height, stride, format_, quality = struct.unpack_from(
            "<HHHHIHH", payload
        )
        return {
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "stride": stride,
            "format": format_,
            "quality": quality,
        }

    async def screen_description(self) -> dict[str, int]:
        """Negotiate full-screen geometry without transferring pixel data."""
        frame = await self._request(Channel.SCREEN, MediaType.OPEN,
                                    self._encode_media_description({}))
        try:
            if frame.type != MediaType.OPENED or len(frame.payload) != 20:
                raise ProtocolError("unexpected screen description OPEN response")
            return self._decode_media_description(frame.payload)
        finally:
            with contextlib.suppress(Exception):
                await self._request(Channel.SCREEN, MediaType.CLOSE, stream_id=frame.stream_id)

    async def screenshot(
        self, description: dict[str, int] | None = None
    ) -> tuple[dict[str, int], bytes]:
        requested = description or {}
        frame = await self._request(
            Channel.SCREEN,
            MediaType.OPEN,
            self._encode_media_description(requested),
        )
        result = bytearray()
        deadline = time.monotonic() + 600.0
        try:
            if frame.type != MediaType.OPENED or len(frame.payload) != 20:
                raise ProtocolError("unexpected screenshot OPEN response")
            actual = self._decode_media_description(frame.payload)
            total_size = struct.unpack_from("<I", frame.payload, 16)[0]
            if frame.stream_id == 0 or not 0 < total_size <= 16 * 1024 * 1024:
                raise ProtocolError("invalid screenshot ID or total size")
            while len(result) < total_size:
                if time.monotonic() >= deadline:
                    raise TimeoutError("screenshot exceeded its overall deadline")
                assert self.info is not None
                maximum = min(
                    max(self.info.max_payload - 64, 1),
                    0xFFFF,
                    total_size - len(result),
                )
                chunk_frame = await self._request(
                    Channel.SCREEN,
                    MediaType.READ,
                    struct.pack("<IHH", len(result), maximum, 0),
                    timeout=min(10.0, max(0.001, deadline - time.monotonic())),
                    stream_id=frame.stream_id,
                )
                if chunk_frame.type != MediaType.DATA or len(chunk_frame.payload) < 8:
                    raise ProtocolError("unexpected screenshot DATA response")
                offset, returned_total = struct.unpack_from(
                    "<II", chunk_frame.payload
                )
                finished = bool(chunk_frame.flags & (1 << 4))
                chunk = chunk_frame.payload[8:-4] if finished else chunk_frame.payload[8:]
                if (
                    offset != len(result)
                    or chunk_frame.stream_id != frame.stream_id
                    or len(chunk) > maximum
                    or offset + len(chunk) > total_size
                    or finished != (offset + len(chunk) == total_size)
                    or returned_total != total_size
                    or not chunk
                ):
                    raise ProtocolError("invalid screenshot chunk")
                result.extend(chunk)
                if finished:
                    expected_crc = struct.unpack_from("<I", chunk_frame.payload, len(chunk_frame.payload) - 4)[0]
                    if zlib.crc32(result) != expected_crc:
                        raise ProtocolError("screenshot whole-object CRC32 mismatch")
        finally:
            with contextlib.suppress(Exception):
                await self._request(Channel.SCREEN, MediaType.CLOSE, stream_id=frame.stream_id)
        return actual, bytes(result)

    async def mirror_start(
        self,
        channel: int,
        description: dict[str, int] | None = None,
        *,
        fps: int = 5,
    ) -> dict[str, Any]:
        if self._console:
            return await self._require_data_link().mirror_start(channel, description, fps=fps)
        if channel not in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            raise ValueError("invalid media channel")
        if not 1 <= fps <= 60:
            raise ValueError("mirror FPS must be between 1 and 60")
        payload = self._encode_media_description(description or {}) + struct.pack(
            "<HH", fps, 0
        )
        frame = await self._request(channel, MediaType.MIRROR_START, payload)
        if frame.type != MediaType.MIRROR_STATE or frame.stream_id == 0:
            raise ProtocolError("unexpected mirror start response")
        self._media_streams[int(channel)] = frame.stream_id
        await self._grant_media_credit(channel, 128 * 1024)
        return {
            "channel": int(channel),
            "stream_id": frame.stream_id,
            "fps": fps,
            "description": self._decode_media_description(frame.payload),
        }

    async def mirror_stop(self, channel: int) -> None:
        if self._console:
            await self._require_data_link().mirror_stop(channel)
            return
        if channel not in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            raise ValueError("invalid media channel")
        stream_id = self._media_streams.get(int(channel), 0)
        frame = await self._request(channel, MediaType.MIRROR_STOP, stream_id=stream_id)
        if frame.type != MediaType.MIRROR_STATE or frame.stream_id != stream_id:
            raise ProtocolError("unexpected mirror stop response")
        self._media_streams.pop(int(channel), None)
        self._media_credit[int(channel)] = 0

    async def ota_update(
        self,
        image: bytes,
        *,
        expected_sha256: bytes | None = None,
        project_name: str = "",
        version: str = "",
        timeout: float = 10.0,
        progress_callback: ProgressCallback | None = None,
    ) -> dict[str, Any]:
        if self._console:
            data = await self._wait_data_link(timeout)
            return await data.ota_update(image,
                expected_sha256=expected_sha256, project_name=project_name,
                version=version, timeout=timeout, progress_callback=progress_callback)
        return await ota_transport.perform_ota_update(
            self, image, expected_sha256=expected_sha256,
            project_name=project_name, version=version, timeout=timeout,
            progress_callback=progress_callback,
        )

    async def ota_status(self, *, timeout: float = 10.0) -> dict[str, Any]:
        return await ota_transport.ota_status(self, timeout=timeout)

    @staticmethod
    def _decode_system_update_status(payload: bytes) -> dict[str, Any]:
        return system_update_transport.decode_system_update_status(payload)

    async def system_update_status(self, *, timeout: float = 10.0) -> dict[str, Any]:
        return await system_update_transport.system_update_status(
            self._request, timeout=timeout
        )

    async def system_update_inventory(
        self, *, timeout: float = 10.0
    ) -> dict[str, Any]:
        return await system_update_transport.system_update_inventory(
            self.wait_ready, self._request, timeout=timeout
        )

    async def system_update(
        self,
        bundle: SystemUpdateBundle,
        *,
        operation_id: bytes | None = None,
        timeout: float = system_update_transport.SYSTEM_UPDATE_REQUEST_TIMEOUT,
        progress_callback: ProgressCallback | None = None,
    ) -> dict[str, Any]:
        if self._console:
            data = await self._wait_data_link(timeout)
            return await data.system_update(bundle,
                operation_id=operation_id, timeout=timeout, progress_callback=progress_callback)
        return await system_update_transport.perform_system_update(
            self.wait_ready,
            self._request,
            bundle,
            operation_id=operation_id,
            timeout=timeout,
            progress_callback=progress_callback,
        )

    async def restart(self, delay_ms: int = 250) -> int:
        if not 100 <= delay_ms <= 60000:
            raise ValueError("restart delay must be 100..60000 ms")
        frame = await self._request(
            Channel.CONTROL,
            ControlType.RESTART,
            struct.pack("<I", delay_ms),
        )
        if frame.type != ControlType.RESTART or len(frame.payload) != 4:
            raise ProtocolError("unexpected restart response")
        return struct.unpack("<I", frame.payload)[0]

    async def sync_clock(self, timeout: float = 3.0) -> None:
        t1_ns = time.monotonic_ns()
        frame = await self._request(
            Channel.CONTROL,
            ControlType.TIME_SYNC_REQUEST,
            struct.pack("<Q", t1_ns),
            timeout,
        )
        t4_ns = time.monotonic_ns()
        if frame.type != ControlType.TIME_SYNC_RESPONSE or len(frame.payload) != 24:
            raise ProtocolError("unexpected time sync response")
        echoed_t1, d2_us, d3_us = struct.unpack("<QQQ", frame.payload)
        if echoed_t1 != t1_ns:
            raise ProtocolError("time sync request echo mismatch")
        host_mid_us = ((t1_ns + t4_ns) / 2.0) / 1000.0
        device_mid_us = (d2_us + d3_us) / 2.0
        rtt_us = max(0.0, (t4_ns - t1_ns) / 1000.0 - (d3_us - d2_us))
        if (
            self.clock_uncertainty_us is None
            or rtt_us / 2.0 < self.clock_uncertainty_us
        ):
            self.clock_offset_us = host_mid_us - device_mid_us
            self.clock_uncertainty_us = rtt_us / 2.0

    async def _clock_loop(self) -> None:
        while not self._closed:
            try:
                await self.sync_clock(self._clock_sync_timeout)
            except (
                asyncio.TimeoutError,
                TimeoutError,
                ConnectionError,
                ProtocolError,
                RuntimeError,
            ):
                if self._closed:
                    # Python 3.8 wait_for can deliver the pending request's
                    # failure during cancellation. Do not delay teardown.
                    return
                if (
                    self._console or self._request_lock.locked()
                ):
                    # Keep raw capture open across bootloaders/debug pauses.
                    # Explicit HELLO queries detect a changed boot/session even
                    # when a UART/Serial-JTAG device never re-enumerates.
                    # An active bulk/RPC request owns its own deadline. A
                    # background clock timeout must not abort a long erase or
                    # a transfer contending with other devices on the host.
                    await asyncio.sleep(self._clock_sync_interval)
                    continue
                # A serial read timeout is handled inside SerialLink and is not
                # an EOF. An unanswered protocol request is different: the
                # enumerated CDC endpoint can outlive the recovery firmware
                # that created it. Close that stale, already-authenticated
                # session so this process never keeps writing to old device
                # state after the board has switched to its normal TCP image.
                await self.close()
                return
            await asyncio.sleep(self._clock_sync_interval)

    def estimate_wall_ns(self, device_monotonic_us: int) -> int | None:
        if self.clock_offset_us is None:
            return None
        host_monotonic_us = device_monotonic_us + self.clock_offset_us
        wall_minus_monotonic_ns = time.time_ns() - time.monotonic_ns()
        return int(host_monotonic_us * 1000 + wall_minus_monotonic_ns)

    async def crash_report(self) -> dict[str, Any]:
        frame = await self._request(Channel.CRASH, CrashType.METADATA_REQUEST)
        if frame.channel != Channel.CRASH or frame.type != CrashType.METADATA_RESPONSE:
            raise ProtocolError("unexpected crash metadata response")
        fields = decode_tlv(frame.payload)
        self._crash_chunk_max = tlv_u32(
            fields, TlvTag.CORE_DUMP_CHUNK_MAX, 1024
        )
        core_sha = fields.get(int(TlvTag.CORE_DUMP_ELF_SHA256), b"").decode(
            "ascii", errors="replace"
        )
        assert self.info is not None
        sha_matches = bool(core_sha) and self.info.firmware_sha256.startswith(core_sha)
        sha_complete = bool(tlv_u8(fields, TlvTag.CORE_DUMP_ELF_SHA256_COMPLETE))
        return {
            "device_id": self.info.device_id,
            "boot_id": tlv_u64(fields, TlvTag.BOOT_ID, self.info.boot_id),
            "session_id": self.info.session_id,
            "reset_reason": tlv_u32(fields, TlvTag.RESET_REASON),
            "previous_boot_crash": bool(
                tlv_u8(fields, TlvTag.PREVIOUS_BOOT_CRASH)
            ),
            "crash_count": tlv_u32(fields, TlvTag.CRASH_COUNT),
            "crash_limit": tlv_u32(fields, TlvTag.CRASH_LIMIT),
            "crash_loop_triggered": bool(
                tlv_u8(fields, TlvTag.CRASH_LOOP_TRIGGERED)
            ),
            "crash_recovery_pending": bool(
                tlv_u8(fields, TlvTag.CRASH_RECOVERY_PENDING)
            ),
            "crash_origin_reset_reason": tlv_u32(
                fields, TlvTag.CRASH_ORIGIN_RESET_REASON
            ),
            "crash_failed_app_address": tlv_u32(
                fields, TlvTag.CRASH_FAILED_APP_ADDRESS
            ),
            "crash_failed_boot_id": tlv_u64(
                fields, TlvTag.CRASH_FAILED_BOOT_ID
            ),
            "crash_failed_firmware_sha256": fields.get(
                int(TlvTag.CRASH_FAILED_FIRMWARE_SHA256), b""
            ).hex(),
            "crash_state_error": tlv_u32(fields, TlvTag.CRASH_STATE_ERROR),
            "core_dump_present": bool(tlv_u8(fields, TlvTag.CORE_DUMP_PRESENT)),
            "core_dump_valid": bool(tlv_u8(fields, TlvTag.CORE_DUMP_VALID)),
            "core_dump_size": tlv_u32(fields, TlvTag.CORE_DUMP_SIZE),
            "core_dump_chunk_max": self._crash_chunk_max,
            "core_dump_elf_sha256": core_sha,
            "core_dump_elf_sha256_complete": sha_complete,
            "firmware_sha256": self.info.firmware_sha256,
            "firmware_sha_matches": sha_matches,
            "decode_eligible": sha_complete and sha_matches,
            "panic_reason": fields.get(int(TlvTag.PANIC_REASON), b"").decode(
                "utf-8", errors="replace"
            ),
        }

    async def read_core_dump_chunk(
        self, offset: int, maximum: int = 1024
    ) -> tuple[int, bytes]:
        if not 0 <= offset <= 0xFFFFFFFF or not 1 <= maximum <= 2048:
            raise ValueError("invalid coredump chunk range")
        maximum = min(maximum, self._crash_chunk_max)
        request = struct.pack("<IHH", offset, maximum, 0)
        frame = await self._request(Channel.CRASH, CrashType.READ_REQUEST, request)
        if frame.channel != Channel.CRASH or frame.type != CrashType.READ_RESPONSE:
            raise ProtocolError("unexpected coredump read response")
        if len(frame.payload) < 8:
            raise ProtocolError("short coredump read response")
        returned_offset, total_size = struct.unpack_from("<II", frame.payload)
        if returned_offset != offset:
            raise ProtocolError("coredump offset mismatch")
        return total_size, frame.payload[8:]
