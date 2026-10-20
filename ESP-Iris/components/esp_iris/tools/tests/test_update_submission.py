"""Exercise real HTTP admission with nine clients and no duplicated writes."""
import asyncio
import json
from collections import Counter
from unittest.mock import Mock

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.cli import _response_json
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.store import GatewayStore
from iris_gateway.update_submission import GatewayHTTPError, submit_system_update


def test_cli_emits_rejected_submission_as_structured_json(monkeypatch, capsys):
    from iris_gateway import cli

    async def rejected(args):
        error = GatewayHTTPError(429, {"code": "resource_limit", "message": "busy",
                                 "details": {"resource": "large_requests"}}, "1")
        error.operation_id = "intent"
        raise error

    monkeypatch.setattr(cli, "_ctl", rejected)
    assert cli.main(["ctl", "--json", "health"]) == 2
    captured = capsys.readouterr()
    result = json.loads(captured.out)
    assert result["submission_state"] == "rejected"
    assert result["write_started"] is False
    assert result["operation_id"] == "intent"
    assert result["error"]["details"]["resource"] == "large_requests"
    assert not captured.err


def test_nine_uploads_wait_for_admission_and_submit_once(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="admission")
        app = create_app(service)
        writes, rejections = Counter(), []

        async def update(request):
            operation_id = request.headers["X-Operation-ID"]
            assert await request.read() == b"frozen bundle"
            writes[operation_id] += 1
            await asyncio.sleep(.01)
            return web.json_response({"operation_id": operation_id})

        async def decode(response):
            try:
                return await _response_json(response)
            except GatewayHTTPError as error:
                rejections.append(error)
                error.retry_after = "0"
                raise

        app.router.add_post("/test/system-update", update)
        client = TestClient(TestServer(app))
        await client.start_server()
        try:
            results = await asyncio.gather(*[
                submit_system_update(client.session, str(client.make_url("/test/system-update")),
                                     b"frozen bundle", {"X-Operation-ID": str(index)},
                                     False, decode)
                for index in range(9)
            ])
            assert len(results) == 9 and rejections
            assert writes == Counter({str(index): 1 for index in range(9)})
            assert all(error.admission_rejected for error in rejections)
            assert service.resource_usage["large_requests"] == 0
        finally:
            await client.close()
            store.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("status,resource", [(429, "large_requests"), (429, "operations"),
                                            (503, "large_requests")])
def test_exhausted_or_other_rejections_preserve_error_without_replay(status, resource):
    async def scenario():
        error = GatewayHTTPError(status, {"code": "resource_limit", "message": "busy",
                                 "details": {"resource": resource}}, "1")

        class Response:
            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                return False

        session = Mock()
        session.post.return_value = Response()

        async def decode(response):
            raise error

        with pytest.raises(GatewayHTTPError) as caught:
            await submit_system_update(session, "url", b"bundle", {"X-Operation-ID": "intent"},
                                       False, decode, admission_timeout=0)
        assert session.post.call_count == 1
        assert caught.value.operation_id == "intent"
        assert (caught.value.as_dict().get("submission_state") == "rejected") == (
            status == 429 and resource == "large_requests")
    asyncio.run(scenario())


def test_network_failure_is_never_replayed():
    async def scenario():
        session = Mock()
        session.post.side_effect = ConnectionError("response lost")
        with pytest.raises(ConnectionError):
            await submit_system_update(session, "url", b"bundle", {"X-Operation-ID": "intent"},
                                       False, None)
        assert session.post.call_count == 1
    asyncio.run(scenario())
