from __future__ import annotations

import asyncio
import base64
import struct

import pytest

from iris_gateway.console_protocol import decode_record, encode_record
from iris_gateway.hub import IrisHub
from iris_gateway.protocol import Channel, ControlType, Frame, TlvTag, encode_tlv
from iris_gateway.store import GatewayStore


class ConsoleLink:
    console = True
    endpoint = "fake:late-console"

    def __init__(self):
        self.incoming = asyncio.Queue()
        self.writes = []
        self.closed = False

    async def read(self):
        return await self.incoming.get()

    async def write(self, data):
        self.writes.append(data)
        if data.startswith(b"iris @"):
            frame = decode_record(data, request=True)
            if frame.type == ControlType.HELLO_ACK:
                await self.incoming.put(encode_record(Frame(channel=Channel.CONTROL,
                    type=ControlType.AUTH_RESULT, session_id=41, sequence=1,
                    payload=b"\x01"), request=False))

    async def close(self):
        self.closed = True


def test_slow_application_keeps_one_reader_and_boot_bytes(tmp_path):
    async def scenario():
        events = []
        store = GatewayStore(tmp_path)
        async def sink(event):
            events.append(event)
            category = "log" if event["kind"] == "log" else "console"
            store.append_event(category, event, event.get("device_id"))
        hub = IrisHub(hello_timeout_seconds=0.01, event_sink=sink)
        link = ConsoleLink()
        opens = 0
        async def opener():
            nonlocal opens
            opens += 1
            return link
        hub._add_supervisor(link.endpoint, opener)
        boot = b"ESP-ROM boot\r\nI (2) bootloader: loading app\r\n"
        try:
            await link.incoming.put(boot)
            for _ in range(100):
                if hub._endpoint_states[link.endpoint]["state"] == "console_only": break
                await asyncio.sleep(0.002)
            assert hub._endpoint_states[link.endpoint]["state"] == "console_only"
            assert opens == 1 and not link.closed
            first = next(item for item in events if item["kind"] == "console_raw")
            assert first["offset"] == 0 and first["device_id"] is None
            assert base64.b64decode(first["data_base64"]) == boot
            await link.incoming.put(encode_record(Frame(channel=Channel.CONTROL,
                type=ControlType.HELLO, session_id=41, payload=encode_tlv([
                    (TlvTag.DEVICE_ID, b"d" * 16), (TlvTag.BOOT_ID, struct.pack("<Q", 77)),
                    (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 2)), (TlvTag.LINK_ROLE, b"\0"),
                ])), request=False))
            for _ in range(100):
                if any(item["kind"] == "console_binding" for item in events): break
                await asyncio.sleep(0.002)
            binding = next(item for item in events if item["kind"] == "console_binding")
            records = store.latest_events(device_id=(b"d" * 16).hex(), capture_id=binding["capture_id"])
            first_raw = next(item for item in records if item["kind"] == "console_raw")
            assert first_raw["boot_id"] is None
            assert first_raw["device_id"] == (b"d" * 16).hex()
            assert opens == 1 and not link.closed
            paths = list((tmp_path / "events").glob("segment-*.sqlite3"))
            assert len(paths) == 1
            assert not list((tmp_path / "logs").rglob("*.jsonl.gz"))
            assert base64.b64decode(first_raw["data_base64"]) == boot
        finally:
            await hub.close()
            store.close()
    asyncio.run(scenario())


def test_raw_capture_rejects_missing_bytes_and_preserves_rotation_gap(tmp_path):
    store = GatewayStore(tmp_path)
    capture = "a" * 32
    event = {"kind": "console_raw", "capture_id": capture, "endpoint": "usb:uart",
             "offset": 0, "boot_id": None, "data_base64": base64.b64encode(b"ROM\x00\xff").decode()}
    first = store.append_event("console", event)
    with pytest.raises(ValueError, match="offset gap"):
        store.append_event("console", {**event, "offset": 10})
    assert len(store.latest_events(endpoint="usb:uart")) == 1
    store.cleanup_raw_logs(max_bytes=0)
    assert not list(store.logs_dir.rglob("*.jsonl.gz"))
    assert store.events_after(first["event_id"] - 1, capture_id=capture)[0][0]["data_base64"] == event["data_base64"]
    store.close()
