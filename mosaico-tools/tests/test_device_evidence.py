"""Exercise the product CLI through the real Iris client against a local API fixture."""
from __future__ import annotations

import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

TOOLS_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_ROOT / "tools"))

from mosaico_cli.cli import main
from mosaico_cli.gateway import GatewaySession

DEVICE_ID = "0123456789abcdef0123456789abcdef"
OPERATION_ID = "34316aaf-5c53-49c0-9d71-44ad598f20ce"
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aOioAAAAASUVORK5CYII="
)


@pytest.fixture
def gateway(tmp_path, monkeypatch):
    state = SimpleNamespace(
        requests=[],
        statuses=[{"device_id": DEVICE_ID, "boot_id": 42, "stale": False, "firmware_mode": "normal"}],
        operation={"operation_id": OPERATION_ID, "status": "outcome_unknown"},
        reconciliation={"operation_id": OPERATION_ID, "reconciliation_id": "verification-1",
                        "outcome": "observed_success", "replayed": False,
                        "evidence": {"status": {"device_id": DEVICE_ID, "boot_id": 43,
                                               "firmware_mode": "recovery", "app_version": "0.2.0"}}},
        capture_error=False,
    )

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, value, code=200):
            body = json.dumps(value).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            state.requests.append(("GET", self.path))
            if self.path == "/v2/devices":
                self.reply({"devices": [{"device_id": DEVICE_ID, "connected": True}]})
            elif self.path == f"/v2/devices/{DEVICE_ID}":
                status = state.statuses.pop(0) if len(state.statuses) > 1 else state.statuses[0]
                self.reply({"device": status})
            elif self.path == f"/v2/operations/{OPERATION_ID}":
                self.reply({"operation": state.operation})
            else:
                self.reply({"error": "unexpected route"}, 404)

        def do_POST(self):
            state.requests.append(("POST", self.path))
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if self.path == f"/v2/operations/{OPERATION_ID}/reconcile":
                return self.reply(state.reconciliation, 201)
            if self.path == f"/v2/devices/{DEVICE_ID}/restart":
                return self.reply({"operation": {
                    "device_id": DEVICE_ID, "operation_id": OPERATION_ID, "status": "succeeded",
                }, "restart": {"previous_boot_id": 42, "boot_id": 43, "reconnected": True}})
            if self.path != f"/v2/devices/{DEVICE_ID}/screenshot?save=true":
                return self.reply({"error": "unexpected route"}, 404)
            if state.capture_error:
                return self.reply({"error": "capture unavailable"}, 503)
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(PNG)))
            self.send_header("X-Operation-ID", OPERATION_ID)
            self.send_header("X-Saved-Artifact", "screenshot-artifact")
            self.send_header("X-ESP-Iris-Media", json.dumps({"width": 1, "height": 1}))
            self.end_headers()
            self.wfile.write(PNG)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    session = GatewaySession(
        Path(sys.executable),
        TOOLS_ROOT.parent / "ESP-Iris/components/esp_iris/tools/esp_iris.py",
        ("--url", f"http://127.0.0.1:{server.server_port}"), None, False,
    )
    state.ensure = Mock(return_value=session)
    state.record = Mock()
    monkeypatch.setattr("mosaico_cli.recovery.record_recovery_verification", state.record)
    monkeypatch.setattr("mosaico_cli.evidence.ensure_gateway", state.ensure)
    monkeypatch.setattr("mosaico_cli.cli.load_workspace", lambda *a, **kw: SimpleNamespace(
        run_dir=tmp_path / "runs", root=tmp_path,
    ))
    # Authentication state from the developer's machine must not enter the fixture.
    monkeypatch.setenv("ESP_IRIS_PROFILE_FILE", str(tmp_path / "profiles.json"))
    monkeypatch.delenv("ESP_IRIS_AGENT_TOKEN", raising=False)
    monkeypatch.delenv("ESP_IRIS_AGENT_TOKEN_FILE", raising=False)
    try:
        yield state
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def test_screenshot_saves_device_bytes_and_correlated_evidence(gateway, tmp_path, capsys):
    output = tmp_path / "captures/device.png"
    assert main(["iris", "screenshot", str(output), "--project", "projects/app", "--json"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert output.read_bytes() == PNG
    assert result["device_id"] == DEVICE_ID
    assert result["boot_id"] == 42
    assert result["capture"]["operation_id"] == OPERATION_ID
    assert result["capture"]["saved_artifact"] == "screenshot-artifact"
    assert gateway.requests == [
        ("GET", "/v2/devices"), ("GET", f"/v2/devices/{DEVICE_ID}"),
        ("POST", f"/v2/devices/{DEVICE_ID}/screenshot?save=true"),
        ("GET", f"/v2/devices/{DEVICE_ID}"),
    ]
    assert gateway.ensure.call_args.kwargs == {"select": True}


@pytest.mark.parametrize("change", [{"stale": True}, {"device_id": "other"}, {"boot_id": None}])
def test_invalid_live_status_never_starts_capture(gateway, tmp_path, capsys, change):
    gateway.statuses[0].update(change)
    output = tmp_path / "device.png"
    assert main(["iris", "screenshot", str(output), "--json"]) == 4
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert not output.exists()
    assert all(method == "GET" for method, _ in gateway.requests)


def test_capture_across_reboot_is_not_success(gateway, tmp_path, capsys):
    gateway.statuses.append({**gateway.statuses[0], "boot_id": 43})
    output = tmp_path / "device.png"
    assert main(["iris", "screenshot", str(output), "--json"]) == 4
    error = json.loads(capsys.readouterr().err)
    assert error["details"]["before"]["boot_id"] == 42
    assert error["details"]["after"]["boot_id"] == 43
    assert error["details"]["capture"]["path"] == str(output)


def test_capture_failure_does_not_fall_back_or_retry(gateway, tmp_path, capsys):
    gateway.capture_error = True
    output = tmp_path / "device.png"
    assert main(["iris", "screenshot", str(output), "--json"]) == 4
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert not output.exists()
    assert len(gateway.requests) == 3


def test_explicit_missing_device_never_captures_another(gateway, tmp_path, capsys):
    assert main(["iris", "screenshot", str(tmp_path / "device.png"), "--device-id", "other", "--json"]) == 4
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert gateway.requests == [("GET", "/v2/devices")]


@pytest.mark.parametrize("json_output", [False, True])
def test_device_status_reports_live_fields(gateway, capsys, json_output):
    assert main(["iris", "device-status", *(["--json"] if json_output else [])]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["device"]["stale"] is False
    assert result["device"]["firmware_mode"] == "normal"
    assert result["boot_id"] == 42


@pytest.mark.parametrize("status", ["running", "failed", "outcome_unknown", "succeeded"])
def test_operation_query_preserves_outcome_without_device_access(gateway, capsys, status):
    gateway.operation["status"] = status
    assert main(["iris", "operation-status", OPERATION_ID, "--json"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["ok"] is True
    assert result["operation"]["status"] == status
    assert gateway.requests == [("GET", f"/v2/operations/{OPERATION_ID}")]
    assert gateway.ensure.call_args.kwargs == {"select": False}


def test_restart_verifies_same_device_and_new_boot(gateway, capsys):
    gateway.statuses.append({**gateway.statuses[0], "boot_id": 43})
    assert main(["iris", "restart", "--json"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["previous_boot_id"] == 42 and result["boot_id"] == 43
    assert result["operation"]["operation_id"] == OPERATION_ID
    assert sum(method == "POST" for method, _ in gateway.requests) == 1


def test_restart_rejects_unchanged_boot_without_retry(gateway, capsys):
    assert main(["iris", "restart", "--json"]) == 4
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert sum(method == "POST" for method, _ in gateway.requests) == 1


def test_restart_rejects_stale_identity_before_write(gateway, capsys):
    gateway.statuses[0]["stale"] = True
    assert main(["iris", "restart", "--json"]) == 4
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert all(method == "GET" for method, _ in gateway.requests)


@pytest.mark.parametrize("outcome", ["observed_success", "outcome_unknown"])
def test_operation_reconcile_only_verifies_original_target(gateway, capsys, outcome):
    gateway.reconciliation["outcome"] = outcome
    succeeded = outcome == "observed_success"
    assert main(["iris", "operation-reconcile", OPERATION_ID, "--json"]) == (0 if succeeded else 5)
    captured = capsys.readouterr()
    result = json.loads(captured.out if succeeded else captured.err)
    assert result["ok"] is succeeded
    assert gateway.requests == [("POST", f"/v2/operations/{OPERATION_ID}/reconcile")]
    assert gateway.ensure.call_args.kwargs == {"select": False}
    if succeeded:
        gateway.record.assert_called_once_with(DEVICE_ID, "0.2.0", 43)
        assert result["reconciliation"]["replayed"] is False
    else:
        gateway.record.assert_not_called()
        assert result["error"] == "outcome_unknown"
        assert result["details"]["reconciliation"]["outcome"] == "outcome_unknown"
