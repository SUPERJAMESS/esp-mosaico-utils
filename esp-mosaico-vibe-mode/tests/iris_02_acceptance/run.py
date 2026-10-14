"""Supplemental 0.2 acceptance through a Gateway retained by mosaico.py.

No direct port access or firmware installation. Explicitly selected installed
fixtures receive RPCs and control fixtures are reset. Raw HTTP/WS evidence and
failed cases remain in the output dir.
"""
import argparse
import asyncio
import base64
import hashlib
import json
from pathlib import Path
import struct
import time
import uuid

import aiohttp


class Acceptance:
    def __init__(self, args):
        self.args = args
        self.root = args.output
        self.root.mkdir(parents=True, exist_ok=False)
        self.report = {"status": "running", "cases": [], "gateway": args.gateway}

    def record(self, name, value):
        (self.root / name).write_text(json.dumps(value, indent=2) + "\n")

    async def request(self, path, body=None, *, expected=200, operation=None):
        headers = {"X-Operation-ID": operation or str(uuid.uuid4())}
        start = time.monotonic()
        async with self.client.request("GET" if body is None else "POST",
                                       self.args.gateway + "/v2" + path,
                                       json=body, headers=headers) as response:
            raw = await response.read()
            image = response.content_type.startswith("image/")
            result = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                      "media": json.loads(response.headers["X-ESP-Iris-Media"])} if image else json.loads(raw)
            entry = {"path": path, "status": response.status, "seconds": time.monotonic() - start,
                     "operation_id": headers["X-Operation-ID"], "response": result}
            with (self.root / "http.jsonl").open("a") as stream:
                stream.write(json.dumps(entry) + "\n")
            assert response.status == expected, entry
            if image:
                assert raw.startswith(b"\x89PNG\r\n\x1a\n")
                result["dimensions"] = struct.unpack_from(">II", raw, 16)
                filename = headers["X-Operation-ID"] + ".png"
                (self.root / filename).write_bytes(raw)
                result["artifact"] = filename
            return result

    async def status(self, device):
        result = await self.request("/devices/" + device)
        assert result["device_id"] == device and not result.get("stale")
        return result

    async def rpc(self, device, service, method, payload=b"", **kwargs):
        deadline = kwargs.pop("deadline", 5000)
        return await self.request(f"/devices/{device}/rpc/raw", {
            "service_id": service, "method_id": method, "deadline_ms": deadline,
            "payload_base64": base64.b64encode(payload).decode()}, **kwargs)

    async def counts(self):
        result = await self.rpc(self.args.device, 0x6A02, 3)
        return struct.unpack("<III", base64.b64decode(result["payload_base64"]))

    async def case(self, name, action):
        entry = {"name": name, "status": "running"}
        self.report["cases"].append(entry)
        start = time.monotonic()
        try:
            entry["evidence"] = await action()
            entry["status"] = "passed"
        except Exception as error:
            entry.update(status="failed", error=str(error))
            raise
        finally:
            entry["seconds"] = time.monotonic() - start
            self.record("result.json", self.report)

    async def slow_control(self):
        before = await self.counts()
        started = time.monotonic()
        slow = asyncio.create_task(self.rpc(self.args.device, 0x6A02, 1))
        latencies = []
        try:
            await asyncio.sleep(.2)
            while not slow.done():
                tick = time.monotonic()
                status = await self.status(self.args.device)
                assert status["boot_id"] == self.initial["boot_id"]
                latencies.append(time.monotonic() - tick)
                await asyncio.sleep(.05)
            await slow
        finally:
            await asyncio.gather(slow, return_exceptions=True)
        assert len(latencies) >= 5 and max(latencies) < 1.0, latencies
        assert (await self.counts())[0] == before[0] + 1
        return {"control_seconds": latencies, "slow_seconds": time.monotonic() - started}

    async def rpc_boundaries(self):
        before = await self.counts()
        for _ in range(10):
            response = await self.rpc(self.args.device, 0x6A02, 2, expected=409)
            assert "payload_base64" not in response
        after = await self.counts()
        assert after[1] == before[1] + 10 and after[0] == before[0]
        return {"before": before, "after": after}

    async def idempotency(self):
        before = await self.counts()
        operation = str(uuid.uuid4())
        results = await asyncio.gather(*[
            self.rpc(self.args.device, 0x6A02, 1, operation=operation) for _ in range(2)])
        assert sum(bool(item.get("idempotent_reuse")) for item in results) == 1
        assert (await self.counts())[0] == before[0] + 1
        conflict = await self.rpc(self.args.device, 0x6A02, 3, operation=operation, expected=409)
        assert conflict["error"]["code"] == "operation_id_conflict"
        return {"operation_id": operation, "responses": results}

    async def timed_out_side_effect(self):
        before = await self.counts()
        operation = str(uuid.uuid4())
        failure = await self.rpc(self.args.device, 0x6A02, 1, deadline=100, operation=operation, expected=409)
        assert "0x00000107" in failure["error"]["message"]  # ESP_ERR_TIMEOUT from device
        await asyncio.sleep(2.2)
        after = await self.counts()
        # The already-running callback can complete after its response deadline.
        assert after[0] == before[0] + 1
        replay = await self.rpc(self.args.device, 0x6A02, 1, deadline=100, operation=operation)
        assert replay["idempotent_reuse"] and replay["operation"]["status"] == "failed"
        assert replay["payload_base64"] is None
        assert (await self.counts())[0] == after[0]
        return {"operation_id": operation, "before": before, "after": after}

    async def multi_device(self):
        before = {device: await self.status(device) for device in self.args.control_device}
        assert len(before) >= 2
        for status in before.values():
            assert status["project_name"].startswith("esp_iris_services_")

        async def echo(device):
            timings = []
            for index in range(40):
                payload = (device + ":" + str(index)).encode().ljust(256, b".")
                start = time.monotonic()
                result = await self.rpc(device, 1, 1, payload)
                assert base64.b64decode(result["payload_base64"]) == payload
                timings.append(time.monotonic() - start)
            image = await self.request(f"/devices/{device}/screenshot?save=true",
                                       {"path": "control", "width": 32, "height": 32})
            assert image["dimensions"] == (32, 32)
            assert image["media"]["transfer_path"] == "control"
            # A control-only device must not silently carry bulk streams.
            error = await self.request(f"/devices/{device}/mirror/start", {"channel": "image"}, expected=409)
            assert error["error"]["code"] == "device_request_failed"
            after = await self.status(device)
            assert after["boot_id"] == before[device]["boot_id"]
            return {"device": device, "latencies": timings, "image": image}

        async def probe():
            for _ in range(40):
                assert (await self.status(self.args.device))["boot_id"] == self.initial["boot_id"]
                await asyncio.sleep(.05)

        return await asyncio.gather(*(echo(device) for device in before), probe())

    async def reset_follow(self, device):
        others = [self.args.device, *(item for item in self.args.control_device if item != device)]
        other_boots = {item: (await self.status(item))["boot_id"] for item in others}
        before = await self.status(device)
        history = await self.request("/events?device_id=" + device)
        cursor = max((item["event_id"] for item in history["events"]), default=0)
        events = []
        url = self.args.gateway + f"/v2/events/ws?device_id={device}&cursor={cursor}"
        async with self.client.ws_connect(url) as socket:
            async def collect():
                async for message in socket:
                    if message.type == aiohttp.WSMsgType.TEXT:
                        event = json.loads(message.data)
                        assert event.get("device_id") in (None, device), event
                        events.append(event)
            collector = asyncio.create_task(collect())
            try:
                operation = await self.request("/consoles/reset", {"endpoint": before["endpoint"], "mode": "run"})
                assert operation["reset"]["reader_started_before_reset"]
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    await asyncio.sleep(.3)
                    try:
                        after = await self.status(device)
                        if after["boot_id"] != before["boot_id"]:
                            break
                    except AssertionError:
                        pass
                else:
                    raise AssertionError("reset did not establish a new live boot")
                await asyncio.sleep(1)
            finally:
                await socket.close()
                await collector
        self.record(device + "-follow.json", events)
        raw = b"".join(base64.b64decode(event["data_base64"]) for event in events
                       if event.get("kind") == "console_raw")
        (self.root / (device + "-boot.raw")).write_bytes(raw)
        assert b"ESP-ROM" in raw and b"2nd stage bootloader" in raw
        for item, boot in other_boots.items():
            assert (await self.status(item))["boot_id"] == boot
        return {"before": before["boot_id"], "after": after["boot_id"],
                "events": len(events), "raw_bytes": len(raw), "other_boots": other_boots}

    async def run(self):
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=90)) as self.client:
            self.initial = await self.status(self.args.device)
            self.report["initial"] = self.initial
            assert self.initial["project_name"] == "iris_acceptance"
            try:
                await self.case("slow_rpc_control_responsiveness", self.slow_control)
                await self.case("oversized_response_rejection", self.rpc_boundaries)
                await self.case("concurrent_idempotency_and_conflict", self.idempotency)
                await self.case("timeout_does_not_replay_side_effect", self.timed_out_side_effect)
                if self.args.control_device:
                    await self.case("three_device_concurrent_control", self.multi_device)
                    for device in self.args.control_device:
                        await self.case("filtered_follow_reset_" + device, lambda d=device: self.reset_follow(d))
                self.report["final"] = await self.status(self.args.device)
                assert self.report["final"]["boot_id"] == self.initial["boot_id"]
                self.report["status"] = "passed"
            except Exception:
                self.report["status"] = "failed"
                raise
            finally:
                self.record("result.json", self.report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--device", required=True, help="Installed iris_acceptance Device ID")
    parser.add_argument("--control-device", action="append", default=[], help="Installed H2/C2 services fixture")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(Acceptance(args).run())


if __name__ == "__main__":
    main()
