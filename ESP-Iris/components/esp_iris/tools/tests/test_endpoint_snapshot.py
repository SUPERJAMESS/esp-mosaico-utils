from __future__ import annotations

import asyncio
import threading
from types import SimpleNamespace
from unittest.mock import Mock

from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.store import GatewayStore


def test_endpoint_discovery_does_not_block_health_or_repeat_per_offline_row(tmp_path, monkeypatch):
    async def scenario():
        service = GatewayService(GatewayStore(tmp_path), instance_id="snapshot")
        endpoints = [{"endpoint": f"usb:old-{index}", "state": "disconnected"} for index in range(18)]
        service.attach_hub(SimpleNamespace(list_endpoints=lambda: endpoints, active_mirrors=lambda _: []))
        entered, release = threading.Event(), threading.Event()

        def discover(**kwargs):
            entered.set()
            assert release.wait(5)
            return []

        discovery = Mock(side_effect=discover)
        lookup = Mock(side_effect=AssertionError("repeated USB enumeration"))
        monkeypatch.setattr("iris_gateway.gateway.discover_iris_usb_devices", discovery)
        monkeypatch.setattr("iris_gateway.device_state.resolve_usb_port", lookup)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        pending = asyncio.create_task(client.get("/v2/endpoints"))
        try:
            while not entered.is_set():
                await asyncio.sleep(0.005)
            response = await asyncio.wait_for(client.get("/v2/health"), 0.5)
            assert response.status == 200 and not release.is_set()
            release.set()
            response = await pending
            rows = (await response.json())["endpoints"]
            assert len(rows) == 18 and all(not item["present"] for item in rows)
            discovery.assert_called_once()
            lookup.assert_not_called()
        finally:
            release.set()
            await pending
            await client.close()
            service.store.close()

    asyncio.run(scenario())
