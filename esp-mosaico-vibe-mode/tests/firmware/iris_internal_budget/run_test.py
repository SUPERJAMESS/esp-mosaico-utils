"""Exercise the fixture only through the Gateway managed by mosaico.py.

Run with the prepared ESP-Iris host Python (aiohttp required). No direct USB
access or firmware writes. Every API response/operation ID is retained.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import struct
import sys
import time
import urllib.request
from pathlib import Path

import aiohttp

TOOLS = Path(__file__).resolve().parents[4] / "mosaico-tools"
sys.path.insert(0, str(TOOLS / "tools"))
from map_report import summarize
from mixed_links import MixedLinks
from mosaico_cli.runtime import RunContext
from mosaico_cli.workspace import load_workspace


async def exercise(args, context, url, initial):
    context.note(json.dumps({"initial_status": initial}, sort_keys=True))
    device_id = initial["device_id"]
    prefix = url + "/v2/devices/" + device_id
    timeout = aiohttp.ClientTimeout(total=60)
    async with aiohttp.ClientSession(timeout=timeout) as client:
        async def request(path, body=None, expected_status=200):
            async with client.request("GET" if body is None else "POST",
                                      prefix + path, json=body) as response:
                data = await response.read()
                if response.status != expected_status:
                    context.note(f"HTTP {response.status} {path}: {data.decode(errors='replace')}")
                    raise RuntimeError(f"HTTP {response.status}: {path}")
                if response.content_type.startswith("image/"):
                    filename = context.directory / f"screen-{time.time_ns()}.png"
                    filename.write_bytes(data)
                    result = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                              "operation_id": response.headers.get("X-Operation-ID"),
                              "media": response.headers.get("X-ESP-Iris-Media"),
                              "artifact": str(filename)}
                else:
                    result = json.loads(data)
                context.note(json.dumps({"path": path, "result": result}, sort_keys=True))
                return result

        async def rpc(method, payload=b"", service=0x6A03):
            result = await request("/rpc/raw", {"service_id": service, "method_id": method,
                "payload_base64": base64.b64encode(payload).decode(), "deadline_ms": 10000})
            return base64.b64decode(result["payload_base64"], validate=True)

        async def metrics():
            if args.production_check:
                sample = await request("/memory")
                if sample["boot_id"] != initial["boot_id"]:
                    raise RuntimeError("device rebooted during memory observation")
                return sample  # Whole-device observation, not Iris heap attribution.
            raw = await rpc(2)
            return dict(zip(("current_heap_bytes", "peak_heap_bytes", "live_blocks",
                "allocations", "trace_errors", "service_stack_free_min_bytes",
                "usb_stack_free_min_bytes"), struct.unpack("<7I", raw)))

        snapshots = {"before": await metrics()}
        state = json.loads(await rpc(1, service=0x1200))
        if (state.get("ota_writer") is not (args.firmware_mode == "recovery") or
                state.get("mode") != args.firmware_mode):
            raise RuntimeError("application recovery state mismatch")
        await request("/system-inventory")
        snapshots["after_inventory"] = await metrics()
        payload = bytes(range(256)) * 4
        async def check_rpc():
            if args.production_check:
                value = json.loads(await rpc(1, service=0x1200))
                return value.get("project") == args.application_name and value.get("mode") == args.firmware_mode
            return await rpc(1, payload) == payload
        for _ in range(args.rpc_count):
            if not await check_rpc():
                raise RuntimeError("1024-byte RPC echo mismatch")
        screenshots = []
        for _ in range(args.screenshots):
            screenshots.append(await request("/screenshot?save=true", {"path": args.snapshot_path}))
        snapshots["after_screenshots"] = await metrics()

        chunks = frames = received = 0
        complete_frames = 0
        async with MixedLinks(client, url, args.control_device, context) as mixed, \
                client.ws_connect(prefix + "/streams/screen") as websocket:
            await request("/mirror/start", {"channel": "screen", "fps": args.fps})
            async def collect():
                nonlocal chunks, frames, received, complete_frames
                until = time.monotonic() + args.seconds
                frame_id = None
                next_y = 0
                while time.monotonic() < until:
                    try:
                        message = await websocket.receive(timeout=1)
                    except asyncio.TimeoutError:
                        continue
                    if message.type == aiohttp.WSMsgType.BINARY:
                        n = int.from_bytes(message.data[:4], "little")
                        meta = json.loads(message.data[4:4+n])
                        tile = meta.get("description", {})
                        y = tile.get("y")
                        height = tile.get("height", 0)
                        chunks += 1
                        received += len(message.data) - 4 - n
                        if y == 0:
                            frames += 1
                            frame_id = meta.get("frame_id")
                            next_y = 0
                        if (frame_id == meta.get("frame_id") and y == next_y and
                                tile.get("width") == 480 and tile.get("stride") == 960 and
                                len(message.data) - 4 - n == 960 * height):
                            next_y += height
                            if next_y == 480:
                                complete_frames += 1
                        else:
                            frame_id = None
                    elif message.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSED):
                        raise RuntimeError("mirror websocket closed early")

            collector = asyncio.create_task(collect())
            try:
                for _ in range(args.rpc_count):
                    if not await check_rpc():
                        raise RuntimeError("RPC echo mismatch during mirror")
                if args.snapshot_path == "control":
                    denied = await request("/screenshot?save=true", {"path": "control"}, expected_status=409)
                    if denied.get("error", {}).get("message") != "stop the active mirror before taking a control-link snapshot":
                        raise RuntimeError("unexpected control snapshot rejection during mirror")
                # An active mirror owns the screen capture; the data path reuses
                # a complete frame without stopping the stream.
                screenshots.append(await request("/screenshot?save=true", {"path": "data"}))
                await request("/system-inventory")
                snapshots["during_mirror"] = await metrics()
                await collector
            finally:
                collector.cancel()
                await request("/mirror/stop", {"channel": "screen"})
        snapshots["after"] = await metrics()
        screenshots.append(await request("/screenshot?save=true", {"path": args.snapshot_path}))
        if chunks == 0 or complete_frames == 0:
            raise RuntimeError("mirror delivered no complete 480x480 RGB565 frame")
        status = await request("")
        live = status.get("device", status)
        if live.get("boot_id") != initial["boot_id"]:
            raise RuntimeError("device rebooted during stress test")
        if (live.get("invalid_frames", 0) != initial.get("invalid_frames", 0) or
                live.get("crash_count", 0) != initial.get("crash_count", 0)):
            raise RuntimeError("invalid-frame or crash counter changed during workload")
        if args.production_check:
            layout = total = None
            passed = True  # Behavior only; never claim a production heap measurement.
        else:
            layout = summarize(args.map.read_text())
            peak = max(x["peak_heap_bytes"] for x in snapshots.values())
            errors = max(x["trace_errors"] for x in snapshots.values())
            total = peak + layout["static_internal_data_bytes"] + layout["resident_iram_bytes"]
            passed = total < 25000 and errors == 0 and min(
                x["usb_stack_free_min_bytes"] for x in snapshots.values()) >= 512
        report = {"device_id": device_id, "boot_id": initial["boot_id"],
            "initial_status": initial, "final_status": live,
            "firmware_sha256": initial["firmware_sha256"], "snapshots": snapshots,
            "static": layout, "peak_internal_bytes": total,
            "limit_bytes": None if args.production_check else 25000,
            "budget_measured": not args.production_check, "passed": passed,
            "rpc_calls": args.rpc_count * 2,
            "rpc_payload_bytes": 0 if args.production_check else 1024,
            "screenshots": screenshots, "mirror_seconds": args.seconds,
            "mirror_chunks": chunks, "mirror_frame_starts": frames, "mirror_bytes": received,
            "mirror_complete_frames": complete_frames, "mixed_links": mixed.report,
            "raw_log": str(context.log_path)}
        (context.directory / "result.json").write_text(json.dumps(report, indent=2, sort_keys=True))
        print(json.dumps(report, sort_keys=True))
        return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--gateway", required=True, help="URL of the project Gateway retained by mosaico.py iris run")
    parser.add_argument("--application-name", required=True, help="Generated production application identity")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--production-check", action="store_true",
                        help="Check installed production UI and whole-device memory, without heap attribution")
    parser.add_argument("--firmware-mode", choices=("normal", "recovery"), default="normal")
    parser.add_argument("--snapshot-path", choices=("auto", "control", "data"), default="auto")
    parser.add_argument("--map", type=Path, default=Path(__file__).parent / "build/iris_internal_budget.map")
    parser.add_argument("--rpc-count", type=int, default=30)
    parser.add_argument("--screenshots", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--fps", type=int, default=5)
    parser.add_argument("--control-device", action="append", default=[],
                        help="Same-Gateway UART/USJ services fixture; repeat for mixed load")
    args = parser.parse_args()
    if args.firmware_mode == "recovery" and not args.production_check:
        parser.error("recovery mode requires --production-check; its heap is not instrumented")
    workspace = load_workspace(TOOLS, explicit=str(args.workspace))
    context = RunContext(workspace, "iris-budget-test", json_output=True)
    url = args.gateway.rstrip("/")
    with urllib.request.urlopen(url + "/v2/devices/" + args.device_id, timeout=15) as response:
        device = json.load(response)
    if device.get("device_id") != args.device_id or device.get("stale"):
        raise RuntimeError("Device identity is stale or mismatched")
    expected = args.application_name if args.production_check else "iris_internal_budget"
    if device.get("project_name") != expected:
        raise RuntimeError(f"install {expected} with mosaico.py first")
    return asyncio.run(exercise(args, context, url, device))


if __name__ == "__main__":
    sys.exit(main())
