"""A cold one-shot update must wait for its separately verified USB data link."""
from unittest.mock import Mock, patch

import pytest

from mosaico_cli.errors import DeviceError
from mosaico_cli.gateway import wait_for_data_link


CONTROL = {"device_id": "device-a", "boot_id": 7, "data_available": False,
           "control_link": {"device_id": "device-a", "boot_id": 7}, "data_link": None}


def test_companion_arrival_is_observed_before_submission():
    ready = {**CONTROL, "data_link": {"device_id": "device-a", "boot_id": 7}}
    with patch("mosaico_cli.gateway.connected_devices", side_effect=[[CONTROL], [ready]]) as poll, \
         patch("mosaico_cli.gateway.time.sleep"):
        wait_for_data_link(Mock(), Mock(), CONTROL, timeout=5)
    assert poll.call_count == 2
    assert all(call.kwargs == {"wait_seconds": 0} for call in poll.call_args_list)


def test_missing_companion_fails_before_any_write_can_be_submitted():
    with patch("mosaico_cli.gateway.connected_devices", return_value=[CONTROL]), \
         patch("mosaico_cli.gateway.time.monotonic", side_effect=[0, 0, 1]), \
         patch("mosaico_cli.gateway.time.sleep"):
        with pytest.raises(DeviceError, match="second USB CDC"):
            wait_for_data_link(Mock(), Mock(), CONTROL, timeout=0.1)


def test_reboot_during_connection_does_not_accept_a_replacement_session():
    restarted = {**CONTROL, "boot_id": 8, "data_link": {"boot_id": 8}}
    with patch("mosaico_cli.gateway.connected_devices", return_value=[restarted]):
        with pytest.raises(DeviceError, match="restarted"):
            wait_for_data_link(Mock(), Mock(), CONTROL, timeout=5)


@pytest.mark.parametrize("device", [{"device_id": "legacy"}, {**CONTROL, "data_link": {"boot_id": 7}}])
def test_ready_or_legacy_connection_needs_no_poll(device):
    with patch("mosaico_cli.gateway.connected_devices") as poll:
        wait_for_data_link(Mock(), Mock(), device, timeout=5)
    poll.assert_not_called()
