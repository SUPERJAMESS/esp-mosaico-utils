from pathlib import Path


RECOVERY_DEFAULTS = (
    Path(__file__).resolve().parents[1]
    / "firmware"
    / "vibe-mode"
    / "sdkconfig.recovery.defaults"
)


def test_recovery_bridge_uses_production_origin_and_board_id() -> None:
    defaults = RECOVERY_DEFAULTS.read_text(encoding="utf-8").splitlines()

    assert (
        'CONFIG_IRIS_FACTORY_BRIDGE_SERVER_URL="https://iris-bridge.esp-claw.com"'
        in defaults
    )
    assert 'CONFIG_IRIS_FACTORY_BRIDGE_BOARD_ID="esp-mosaico"' in defaults
