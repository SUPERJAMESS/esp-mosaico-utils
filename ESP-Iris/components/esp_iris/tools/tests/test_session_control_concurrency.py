from __future__ import annotations

import asyncio
from dataclasses import replace
from unittest.mock import AsyncMock

import pytest

from iris_gateway.protocol import Channel, ControlType, Frame, decode_frame
from iris_gateway.session import DeviceInfo, DeviceSession


def ready_session():
    link = AsyncMock()
    link.endpoint = "fake:control-concurrency"
    session = DeviceSession(link, AsyncMock(), AsyncMock())
    session.info = DeviceInfo(
        device_id="device", boot_id=1, session_id=7, endpoint=link.endpoint,
        transport=1, project_name="test", app_version="1", idf_version="6.1",
        firmware_sha256="00" * 32, reset_reason=1, capabilities=0,
        auth_mode=0, max_payload=4000,
    )
    session._ready.set()
    return session, link


def test_status_retries_one_timeout_on_same_session():
    async def scenario():
        session, _ = ready_session()
        response = Frame(channel=Channel.CONTROL, type=ControlType.STATUS_RESPONSE, payload=b"")
        session._request = AsyncMock(side_effect=[asyncio.TimeoutError(), response])
        result = await session.status()
        assert result["device_id"] == "device"
        assert session._request.await_count == 2
        for call in session._request.await_args_list:
            assert call.args == (Channel.CONTROL, ControlType.STATUS_REQUEST)
        session._request = AsyncMock(side_effect=asyncio.TimeoutError())
        with pytest.raises(asyncio.TimeoutError):
            await session.status()
        assert session._request.await_count == 2

    asyncio.run(scenario())


@pytest.mark.parametrize("same_boot", [True, False])
def test_status_retry_uses_protocol_identity_across_periodic_hello(same_boot):
    async def scenario():
        session, _ = ready_session()
        calls = 0

        async def response(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                # Periodic console HELLO replaces DeviceInfo even when its
                # Device ID, Boot ID and protocol session remain unchanged.
                session.info = replace(session.info, boot_id=1 if same_boot else 2)
                raise asyncio.TimeoutError()
            return Frame(channel=Channel.CONTROL, type=ControlType.STATUS_RESPONSE, payload=b"")

        session._request = response
        if same_boot:
            assert (await session.status())["boot_id"] == 1
            assert calls == 2
        else:
            with pytest.raises(asyncio.TimeoutError):
                await session.status()
            assert calls == 1

    asyncio.run(scenario())


@pytest.mark.parametrize("changed", ["closed", "identity"])
def test_status_never_retries_across_changed_session(changed):
    async def scenario():
        session, _ = ready_session()

        async def timeout(*args, **kwargs):
            if changed == "closed":
                session._closed = True
            else:
                session.info = None
            raise asyncio.TimeoutError()

        session._request = AsyncMock(side_effect=timeout)
        with pytest.raises(asyncio.TimeoutError):
            await session.status()
        assert session._request.await_count == 1

    asyncio.run(scenario())


async def wait_for_writes(link, count: int) -> list[Frame]:
    for _ in range(100):
        if link.write.await_count >= count:
            return [decode_frame(call.args[0][:-1]) for call in link.write.await_args_list]
        await asyncio.sleep(0)
    raise AssertionError(f"expected {count} writes, got {link.write.await_count}")


@pytest.mark.parametrize("type_", [ControlType.STATUS_REQUEST, ControlType.PING,
                                   ControlType.TIME_SYNC_REQUEST])
def test_readonly_control_probe_bypasses_pending_rpc(type_) -> None:
    async def scenario() -> None:
        session, link = ready_session()
        rpc = asyncio.create_task(session.rpc(0x6A02, 1, timeout=10))
        await wait_for_writes(link, 1)
        if type_ == ControlType.STATUS_REQUEST:
            probe = asyncio.create_task(session.status())
        else:
            probe = asyncio.create_task(session._request(Channel.CONTROL, type_))
        frames = await wait_for_writes(link, 2)
        assert frames[0].type == ControlType.REQUEST
        assert frames[1].type == type_
        assert frames[0].request_id != frames[1].request_id
        assert frames[1].sequence == frames[0].sequence + 1
        response_type = {
            ControlType.STATUS_REQUEST: ControlType.STATUS_RESPONSE,
            ControlType.PING: ControlType.PONG,
            ControlType.TIME_SYNC_REQUEST: ControlType.TIME_SYNC_RESPONSE,
        }[type_]
        await session._handle_frame(Frame(
            channel=Channel.CONTROL, type=response_type, session_id=7,
            sequence=1, request_id=frames[1].request_id,
        ), 0)
        result = await asyncio.wait_for(probe, 1)
        if type_ == ControlType.STATUS_REQUEST:
            assert result["device_id"] == "device"
        assert not rpc.done()
        await session.close()
        with pytest.raises(ConnectionError, match="session closed"):
            await rpc

    asyncio.run(scenario())


def test_mutating_request_stays_serial_and_close_releases_all_waiters() -> None:
    async def scenario() -> None:
        session, link = ready_session()
        first = asyncio.create_task(session.rpc(0x6A02, 1, timeout=10))
        await wait_for_writes(link, 1)
        queued = asyncio.create_task(session.rpc(0x6A02, 2, timeout=10))
        status = asyncio.create_task(session.status())
        frames = await wait_for_writes(link, 2)
        assert [frame.type for frame in frames] == [ControlType.REQUEST, ControlType.STATUS_REQUEST]
        assert len(session._pending) == 2
        await session.close()
        results = await asyncio.wait_for(asyncio.gather(
            first, queued, status, return_exceptions=True
        ), 1)
        assert all(isinstance(result, ConnectionError) for result in results)
        assert not session._pending
        assert not session._request_lock.locked()
        assert link.write.await_count == 2
        link.close.assert_awaited_once()

    asyncio.run(scenario())


@pytest.mark.parametrize("active_request", [False, True])
def test_clock_timeout_does_not_abort_an_active_data_request(active_request):
    async def scenario():
        session, link = ready_session()
        probed = asyncio.Event()

        async def timeout(*args):
            probed.set()
            raise asyncio.TimeoutError

        session.sync_clock = timeout
        if active_request:
            await session._request_lock.acquire()
        clock = asyncio.create_task(session._clock_loop())
        try:
            await asyncio.wait_for(probed.wait(), 1)
            await asyncio.sleep(0)
            assert link.close.await_count == (0 if active_request else 1)
            assert session._closed is (not active_request)
        finally:
            if active_request:
                session._request_lock.release()
            clock.cancel()
            await asyncio.gather(clock, return_exceptions=True)
            await session.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("kind", ["ota_update", "system_update"])
def test_update_waits_for_independent_data_authentication(kind):
    async def scenario():
        control, _ = ready_session()
        control._console = True
        data, _ = ready_session()
        data.owner_id = control.owner_id
        data._ready.clear()
        control.data_session = data
        transferred = AsyncMock(return_value={"validated": True})
        setattr(data, kind, transferred)
        operation = asyncio.create_task(getattr(control, kind)(b"image", timeout=1))
        try:
            await asyncio.sleep(0)
            transferred.assert_not_awaited()
            data._ready.set()
            assert await operation == {"validated": True}
            transferred.assert_awaited_once()
        finally:
            await control.close()
            await data.close()

    asyncio.run(scenario())


def test_update_data_admission_does_not_accept_wrong_binding():
    async def scenario():
        control, _ = ready_session()
        data, _ = ready_session()
        control._console = True
        control.data_session = data  # Authenticated to a different owner.
        with pytest.raises(ValueError, match="binding"):
            await control._wait_data_link(1)
        await control.close()
        await data.close()

    asyncio.run(scenario())
