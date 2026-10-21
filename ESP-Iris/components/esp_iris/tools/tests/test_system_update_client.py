"""Retry only confirmed pre-admission pressure, preserving operation identity."""
import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from iris_gateway import cli


class Response:
    def __init__(self, status, body):
        self.status, self.body = status, body
        self.headers = {"Retry-After": "0.05"}

    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass
    async def json(self): return self.body


def rejected(resource="large_requests"):
    return Response(429, {"error": {"code": "resource_limit", "message": "busy",
                                    "details": {"resource": resource}}})


def test_system_upload_waits_for_admission_with_same_bytes_and_operation_id(monkeypatch):
    responses = [rejected(), rejected(), Response(202, {"operation_id": "same", "status": "queued"})]
    requests = []

    def post(url, **kwargs):
        requests.append((url, kwargs))
        return responses.pop(0)

    sleep = AsyncMock()
    monkeypatch.setattr(cli.asyncio, "sleep", sleep)
    headers = {"X-Operation-ID": "same"}
    result = asyncio.run(cli._post_update_upload(SimpleNamespace(post=post), "url", None, b"image", headers))
    assert result["status"] == "queued"
    assert len(requests) == 3 and requests[0] == requests[1] == requests[2]
    assert sleep.await_count == 2


@pytest.mark.parametrize("response", [Response(500, {"error": {"message": "unknown"}}),
                                      rejected("operations")])
def test_other_errors_are_not_replayed(response):
    requests = []

    def post(*args, **kwargs):
        requests.append(kwargs)
        return response

    with pytest.raises(RuntimeError, match="HTTP"):
        asyncio.run(cli._post_update_upload(SimpleNamespace(post=post), "url", None, b"image", {}))
    assert len(requests) == 1


def test_exhausted_admission_is_a_definite_rejection(monkeypatch):
    monkeypatch.setattr(cli, "SYSTEM_UPDATE_ADMISSION_ATTEMPTS", 2)
    monkeypatch.setattr(cli.asyncio, "sleep", AsyncMock())
    session = SimpleNamespace(post=lambda *args, **kwargs: rejected())
    with pytest.raises(cli.SystemUpdateAdmissionRejected):
        asyncio.run(cli._post_update_upload(session, "url", None, b"image", {}))


def test_rejected_artifact_upload_rebuilds_consumable_multipart(monkeypatch):
    responses = [rejected(), Response(201, {"artifact": {"id": "image"}})]
    bodies = []

    def post(url, **kwargs):
        bodies.append(kwargs["data"])
        return responses.pop(0)

    monkeypatch.setattr(cli.asyncio, "sleep", AsyncMock())
    session = SimpleNamespace(post=post)
    result = asyncio.run(cli._post_update_upload(session, "url", None, object, {}))
    assert result["artifact"]["id"] == "image"
    assert len(bodies) == 2 and bodies[0] is not bodies[1]
