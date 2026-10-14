"""Concurrent control-only fixture load while the budget fixture streams USB."""
import asyncio
import base64
import hashlib
import json
import struct
import time

import aiohttp


class MixedLinks:
    def __init__(self, client, gateway, devices, context):
        self.client = client
        self.gateway = gateway
        self.devices = devices
        self.context = context
        self.stop = asyncio.Event()
        self.tasks = []
        self.report = {}

    async def request(self, device, path="", body=None):
        async with self.client.request("GET" if body is None else "POST",
                self.gateway + "/v2/devices/" + device + path, json=body) as response:
            raw = await response.read()
            if response.status != 200:
                raise RuntimeError(f"mixed {device} {path}: HTTP {response.status} {raw!r}")
            if response.content_type.startswith("image/"):
                if struct.unpack_from(">II", raw, 16) != (32, 32):
                    raise RuntimeError("control fixture screenshot dimensions changed")
                artifact = self.context.directory / (device + "-control.png")
                artifact.write_bytes(raw)
                result = {"artifact": str(artifact), "sha256": hashlib.sha256(raw).hexdigest(),
                          "media": json.loads(response.headers["X-ESP-Iris-Media"])}
                if result["media"]["transfer_path"] != "control":
                    raise RuntimeError("control fixture used an unexpected data link")
            else:
                result = json.loads(raw)
            self.context.note(json.dumps({"mixed_device": device, "path": path, "result": result}))
            return result

    async def status(self, device):
        value = await self.request(device)
        if value.get("device_id") != device or value.get("stale"):
            raise RuntimeError("mixed fixture identity is stale or mismatched")
        return value

    async def __aenter__(self):
        for device in self.devices:
            initial = await self.status(device)
            if not initial["project_name"].startswith("esp_iris_services_"):
                raise RuntimeError("mixed control device must run the Iris services fixture")
            self.report[device] = {"initial": initial, "rpc_seconds": [], "events": 0}
        self.tasks = [asyncio.create_task(self.load(device)) for device in self.devices]
        return self

    async def load(self, device):
        row = self.report[device]
        events = self.context.directory / (device + "-events.jsonl")
        async with self.client.ws_connect(self.gateway + "/v2/events/ws?device_id=" + device) as socket:
            async def follow():
                async for message in socket:
                    if message.type == aiohttp.WSMsgType.TEXT:
                        event = json.loads(message.data)
                        if event.get("device_id") not in (None, device):
                            raise RuntimeError("cross-device event delivered to filtered subscriber")
                        with events.open("a") as stream:
                            stream.write(json.dumps(event) + "\n")
                        row["events"] += 1
            follower = asyncio.create_task(follow())
            try:
                row["screenshot"] = await self.request(device, "/screenshot?save=true",
                    {"path": "control", "width": 32, "height": 32})
                while not self.stop.is_set():
                    payload = (device + ":" + str(len(row["rpc_seconds"]))).encode().ljust(256, b".")
                    start = time.monotonic()
                    result = await self.request(device, "/rpc/raw", {"service_id": 1, "method_id": 1,
                        "payload_base64": base64.b64encode(payload).decode(), "deadline_ms": 5000})
                    if base64.b64decode(result["payload_base64"], validate=True) != payload:
                        raise RuntimeError("mixed control echo corruption or wrong device response")
                    row["rpc_seconds"].append(time.monotonic() - start)
                    await asyncio.sleep(.02)
            finally:
                await socket.close()
                await follower
        row["final"] = await self.status(device)
        for key in ("boot_id", "crash_count", "invalid_frames"):
            if row["final"].get(key) != row["initial"].get(key):
                raise RuntimeError(f"mixed fixture {device}: {key} changed")
        if not row["rpc_seconds"] or not row["events"]:
            raise RuntimeError("mixed fixture delivered no RPC/log observations")

    async def __aexit__(self, exc_type, exc, tb):
        self.stop.set()
        results = await asyncio.gather(*self.tasks, return_exceptions=True)
        for result in results:
            if isinstance(result, BaseException):
                if exc is None:
                    raise result
                self.context.note("mixed load also failed: " + repr(result))
