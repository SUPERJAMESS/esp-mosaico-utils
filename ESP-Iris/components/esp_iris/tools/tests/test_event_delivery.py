from __future__ import annotations

import asyncio

from iris_gateway.gateway import GatewayService
from iris_gateway.store import GatewayStore


def test_slow_subscriber_cannot_interrupt_other_observers(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="observers")
        slow = service.subscribe()
        fast = service.subscribe()
        try:
            for index in range(slow.maxsize * 2 + 3):
                service._broadcast_event({"kind": "log", "text": str(index), "event_id": index + 1})
                received = fast.get_nowait()
                assert received["text"] == str(index)
                assert received["event_id"] == index + 1
                assert slow.qsize() <= slow.maxsize
            retained = []
            while not slow.empty():
                retained.append(slow.get_nowait())
            assert retained[-1]["text"] == str(index)
            gaps = [item for item in retained if item["kind"] == "history_gap"]
            assert gaps
            missing = index + 1 - sum(item["kind"] != "history_gap" for item in retained)
            assert sum(item["dropped_events"] for item in gaps) == missing
            service.unsubscribe(slow)
            await service.on_device_event({"kind": "log", "text": "still connected"})
            assert (await asyncio.wait_for(fast.get(), 2))["text"] == "still connected"
        finally:
            service.unsubscribe(slow)
            service.unsubscribe(fast)
            store.close()

    asyncio.run(scenario())
