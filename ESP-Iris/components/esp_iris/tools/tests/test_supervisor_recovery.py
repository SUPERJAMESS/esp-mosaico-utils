from __future__ import annotations

import asyncio

import pytest
from iris_gateway.hub import IrisHub
from iris_gateway.link import EndpointLock
from test_hub import SupervisorLink
from test_usb_ownership import until


@pytest.fixture
def endpoint(tmp_path, monkeypatch):
    monkeypatch.setattr("iris_gateway.link.tempfile.gettempdir", lambda: str(tmp_path))
    return "usb:location=recovery-test:1.0"


def test_unexpected_supervisor_failure_retries_with_diagnostics(endpoint, caplog):
    async def scenario():
        hub = IrisHub(reconnect_min_seconds=0.01, reconnect_max_seconds=0.02)
        attempts = 0
        available = False
        link = SupervisorLink(1, endpoint=endpoint)

        async def opener():
            nonlocal attempts
            attempts += 1
            if not available:
                raise TypeError("descriptor disappeared")
            return link

        try:
            hub._add_supervisor(endpoint, opener)
            await until(lambda: attempts >= 3)
            state = hub.list_endpoints()[0]
            assert state["state"] == "retrying"
            assert state["error"] == "TypeError: descriptor disappeared"
            assert state["last_error"] == state["error"]
            available = True
            await until(lambda: bool(hub.list_devices()))
            state = hub.list_endpoints()[0]
            assert state["state"] == "ready"
            assert state["error"] is None
            assert state["last_error"] == "TypeError: descriptor disappeared"
            assert "descriptor disappeared" in caplog.text
        finally:
            await hub.close()
        assert link.closed

    asyncio.run(scenario())


@pytest.mark.parametrize("action", ["reclaim", "release", "detach", "close"])
def test_finished_failed_task_does_not_block_lifecycle(endpoint, action, caplog):
    async def scenario():
        hub = IrisHub()
        links = []

        async def opener():
            link = SupervisorLink(len(links) + 1, endpoint=endpoint)
            links.append(link)
            return link

        async def failed():
            raise TypeError("old supervisor failed")

        try:
            hub._add_supervisor(endpoint, opener)
            await until(lambda: bool(hub.list_devices()))
            # Model a terminal task left by an older gateway or a failure
            # outside the connection retry loop. Keep its physical lock.
            original = hub._endpoint_tasks[endpoint]
            original.cancel()
            await asyncio.gather(original, return_exceptions=True)
            dead = asyncio.create_task(failed())
            hub._endpoint_tasks[endpoint] = dead
            await asyncio.wait({dead})

            if action == "reclaim":
                hub._add_supervisor(endpoint, opener)
                assert hub._endpoint_tasks[endpoint] is not dead
                await until(lambda: bool(hub.list_devices()))
            elif action == "release":
                await hub._remove_endpoint(endpoint)
                assert endpoint not in hub._endpoint_tasks
                assert endpoint not in hub._endpoint_states
                assert endpoint not in hub._endpoint_configs
                assert endpoint not in hub._locks
                probe = EndpointLock(endpoint)
                try:
                    probe.acquire()
                finally:
                    probe.close()
            elif action == "detach":
                result = await hub.detach_for_host(endpoint)
                assert result["state"] == "host_operation"
                assert endpoint in hub._locks
                assert endpoint not in hub._endpoint_tasks
                await hub.resume_after_host(endpoint)
                await until(lambda: bool(hub.list_devices()))
            else:
                await hub.close()
                assert not hub._endpoint_tasks
                assert not hub._locks
            assert "old supervisor failed" in caplog.text
        finally:
            await hub.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("action", ["release", "detach", "close"])
def test_cancelling_failed_supervisor_during_backoff_does_not_restart(endpoint, action):
    async def scenario():
        hub = IrisHub(reconnect_min_seconds=0.02, reconnect_max_seconds=0.04)
        attempts = 0

        async def opener():
            nonlocal attempts
            attempts += 1
            raise ValueError("descriptor conversion failed")

        try:
            hub._add_supervisor(endpoint, opener)
            await until(lambda: hub.list_endpoints()[0]["state"] == "retrying")
            if action == "release":
                await hub._remove_endpoint(endpoint)
            elif action == "detach":
                await hub.detach_for_host(endpoint)
            else:
                await hub.close()
                assert hub.list_endpoints()[0]["state"] == "stopped"
                assert hub.list_endpoints()[0]["error"] == "ValueError: descriptor conversion failed"
            stopped_at = attempts
            await asyncio.sleep(0.06)
            assert attempts == stopped_at
            assert endpoint not in hub._endpoint_tasks
        finally:
            await hub.close()

    asyncio.run(scenario())
