from __future__ import annotations

import asyncio

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.operations import OperationManager
from iris_gateway.resource_limits import (
    ByteBudgetQueue,
    GatewayLimits,
    ResourceExhausted,
)
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore


def test_byte_budget_reports_loss_and_releases_bytes():
    async def scenario():
        queue = ByteBudgetQueue(100, 2048)
        for index in range(100):
            queue.push({"kind": "log", "text": str(index) + "x" * 600})
            assert queue.bytes <= 2048
        retained = []
        while not queue.empty():
            retained.append(queue.get_nowait())
        assert queue.bytes == 0
        assert retained[-1]["text"].startswith("99")
        assert sum(item.get("dropped_events", 1) for item in retained) == 100
        queue.push({"kind": "log", "text": "x" * 10000})
        assert queue.get_nowait()["kind"] == "history_gap"
        assert queue.bytes == 0
    asyncio.run(scenario())


def test_operation_capacity_rejects_before_side_effect_and_releases_on_cancel(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        manager = OperationManager(store, lambda event: asyncio.sleep(0), limits=GatewayLimits(operations=1))
        actor = Actor("local", "test")
        release = asyncio.Event()
        try:
            await manager.submit("a", actor, "rpc.raw", {}, release.wait, operation_id="running")
            existing, created = await manager.submit("a", actor, "rpc.raw", {}, release.wait, operation_id="running")
            assert not created and existing["operation_id"] == "running"
            with pytest.raises(ResourceExhausted):
                await manager.submit("b", actor, "rpc.raw", {}, release.wait, operation_id="rejected")
            assert store.operation("rejected") is None
            await manager.close()
            assert not manager.admission.reservations and not manager._locks
            result, _, _ = await manager.execute("b", actor, "read", {}, lambda: asyncio.sleep(0))
            assert result["status"] == "succeeded"
            with pytest.raises(ResourceExhausted):
                await manager.execute("b", actor, "write", {}, release.wait, cost_bytes=129 * 1024**2)
        finally:
            await manager.close()
            store.close()
    asyncio.run(scenario())


def test_http_capacity_chunked_body_and_subscriber_release(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="budget", limits=GatewayLimits(
            requests=1, json_bytes=256, subscribers=2, subscribers_per_actor=1))
        app = create_app(service)
        entered, release = asyncio.Event(), asyncio.Event()
        async def hold(request):
            entered.set()
            await release.wait()
            return web.json_response({})
        async def read(request):
            await request.read()
            return web.json_response({})
        app.router.add_get("/hold", hold)
        app.router.add_post("/read", read)
        client = TestClient(TestServer(app))
        await client.start_server()
        try:
            first = asyncio.create_task(client.get("/hold"))
            await entered.wait()
            response = await client.get("/read")
            assert response.status == 429 and response.headers["Retry-After"] == "1"
            assert (await client.get("/v2/health")).status == 200
            release.set()
            await first
            async def chunks():
                yield b"x" * 200
                yield b"x" * 200
            assert (await client.post("/read", data=chunks())).status == 413
            assert service.resource_usage["requests"] == 0
            queue = service.subscribe("actor")
            with pytest.raises(ResourceExhausted):
                service.subscribe("actor")
            service.unsubscribe(queue)
            service.unsubscribe(service.subscribe("actor"))
            assert not service._subscribers and not service._subscriber_owners
        finally:
            release.set()
            await client.close()
            store.close()
    asyncio.run(scenario())


def test_cancelled_queue_releases_capacity_before_running_operation_finishes(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        manager = OperationManager(store, lambda event: asyncio.sleep(0), limits=GatewayLimits(operations=2))
        actor = Actor("local", "test")
        started, release = asyncio.Event(), asyncio.Event()
        async def running():
            started.set()
            await release.wait()
        try:
            await manager.submit("a", actor, "read", {}, running, operation_id="first")
            await started.wait()
            await manager.submit("a", actor, "read", {}, release.wait, operation_id="queued")
            assert len(manager.admission.reservations) == 2
            assert await manager.cancel_queued() == 1
            assert not release.is_set() and len(manager.admission.reservations) == 1
            assert store.operation("queued")["status"] == "cancelled"
            operation, _, _ = await manager.execute("b", actor, "read", {}, lambda: asyncio.sleep(0))
            assert operation["status"] == "succeeded"
        finally:
            release.set()
            await manager.close()
            store.close()
    asyncio.run(scenario())
