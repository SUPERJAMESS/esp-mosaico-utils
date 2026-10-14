"""USB Bridge pairing waits for a server code without replaying entry RPCs."""
import argparse
import base64
import json
from pathlib import Path
import sys
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mosaico-tools/tools"))
from mosaico_cli.cli import build_parser
from mosaico_cli.commands import read_bridge_code
from mosaico_cli.errors import DeviceError, OperationError


def wire(**value):
    return {"payload_base64": base64.b64encode(json.dumps(value).encode()).decode()}


def invoke(responses, timeout=60):
    context = mock.Mock(log_path=Path("test.log"))
    session = mock.Mock(started_local=False)
    args = argparse.Namespace(timeout=timeout)
    clock = [0.0]

    def sleep(seconds):
        clock[0] += seconds

    with mock.patch("mosaico_cli.commands._recovery_control_device", return_value=(session, "device-a")), \
            mock.patch("mosaico_cli.commands.gateway_json", side_effect=responses) as rpc, \
            mock.patch("mosaico_cli.commands.time.monotonic", side_effect=lambda: clock[0]), \
            mock.patch("mosaico_cli.commands.time.sleep", side_effect=sleep):
        result = read_bridge_code(args, context)
    return result, rpc, context


def test_waits_for_network_registration_and_code_without_reopening():
    result, rpc, context = invoke([
        wire(state="WAITING_NETWORK", running=True),
        wire(state="REGISTERING", running=True),
        wire(state="PAIRING", running=True, code="ABCDE-12345", expires_in_ms=599000,
             expires_at="2026-09-11T12:10:00Z", server_url="https://flash.example.com"),
    ])
    assert [call.args[5] for call in rpc.call_args_list] == ["4", "5", "5"]
    assert all(call.kwargs["sensitive_output"] for call in rpc.call_args_list)
    assert result["bridge"]["code"] == "ABCDE-12345"
    assert result["server_url"] == "https://flash.example.com"
    assert "ABCDE" not in str(context.status.call_args_list)


@pytest.mark.parametrize("state", ["EXPIRED", "CANCELLED", "ENDED", "FAILED", "IDLE"])
def test_terminal_session_is_not_reopened(state):
    with pytest.raises(DeviceError, match="ended"):
        invoke([wire(state=state, running=False)])


@pytest.mark.parametrize("state", ["PAIRED", "PRECHECK", "WRITING", "VERIFYING", "COMMITTING"])
def test_active_flash_is_not_repaired(state):
    with pytest.raises(DeviceError, match="already paired"):
        invoke([wire(state=state, running=True)])


def test_missing_build_configuration():
    with pytest.raises(DeviceError, match="not configured"):
        invoke([wire(state="NOT_CONFIGURED", running=False)])


def test_timeout_is_bounded():
    with pytest.raises(OperationError, match="timeout"):
        invoke([wire(state="WAITING_NETWORK", running=True)] * 4, timeout=0.5)


def test_final_rpc_exhausting_deadline_reports_pairing_timeout():
    clock = [0.0]
    calls = []

    def rpc(*args, **kwargs):
        calls.append(args[5])
        if len(calls) == 1:
            return wire(state="WAITING_NETWORK", running=True)
        clock[0] += kwargs["timeout"]
        raise DeviceError("The ESP-Iris Gateway request timed out.")

    with mock.patch("mosaico_cli.commands._recovery_control_device", return_value=(mock.Mock(), "device-a")), \
            mock.patch("mosaico_cli.commands.gateway_json", side_effect=rpc), \
            mock.patch("mosaico_cli.commands.time.monotonic", side_effect=lambda: clock[0]), \
            mock.patch("mosaico_cli.commands.time.sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
        with pytest.raises(OperationError, match="pairing code.*timeout") as error:
            read_bridge_code(argparse.Namespace(timeout=0.5), mock.Mock())
    assert calls == ["4", "5"]
    assert error.value.details["bridge"]["state"] == "WAITING_NETWORK"
    assert clock[0] == 0.5


def test_gateway_failure_before_deadline_is_preserved():
    with pytest.raises(DeviceError, match="Gateway unavailable"):
        invoke([DeviceError("Gateway unavailable")])


@pytest.mark.parametrize("code", ["ABCDE-12345", "03827164", "038271", "abcd1234"])
def test_server_code_is_returned_without_format_validation(code):
    result, _, _ = invoke([wire(state="PAIRING", running=True, code=code,
                              server_url="https://flash.example.com")])
    assert result["bridge"]["code"] == code


@pytest.mark.parametrize("code", [12345678, ["12345678"], {"value": "12345678"}])
def test_non_string_code_is_rejected(code):
    with pytest.raises(DeviceError, match="invalid Bridge pairing code"):
        invoke([wire(state="PAIRING", running=True, code=code)])


def test_removed_cli_entries_are_rejected():
    for arguments in [["http-update-code"], ["iris", "system-update", "--manifest-url", "https://example.com/m.json"]]:
        with pytest.raises(SystemExit):
            build_parser().parse_args(arguments)
