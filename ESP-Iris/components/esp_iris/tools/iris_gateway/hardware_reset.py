"""Bounded DTR/RTS sequences matching ESP-IDF Monitor's reset semantics.

The caller owns an already open serial endpoint and starts its reader first.
CTS is an input and is never used to reset the chip. Ordinary reconnect does
not call this module. Sequence values are pySerial assertion booleans.
"""
from __future__ import annotations

import time
from typing import Callable, Iterable, Protocol, Tuple

from .chip_targets import target_from_name

Step = Tuple[str, float]


class SerialControlLines(Protocol):
    """Writable pySerial control lines; CTS is intentionally not a reset output."""

    dtr: bool
    rts: bool


def reset_steps(mode: str, circuit: str, *, chip: str | None = None) -> tuple[Step, ...]:
    if mode not in {"run", "rom", "attach"}:
        raise ValueError("reset mode must be run, rom, or attach")
    if circuit not in {"uart", "usb_serial_jtag"}:
        raise ValueError("hardware reset requires UART DTR/RTS or USB Serial/JTAG")
    if mode == "attach":
        return ()
    if mode == "run":
        return (("D", 0), ("R", 1), ("W", 0.005), ("R", 0))
    if circuit == "usb_serial_jtag":
        return (("R", 0), ("D", 0), ("W", 0.1), ("D", 1), ("R", 0),
                ("W", 0.1), ("R", 1), ("D", 0), ("R", 1), ("W", 0.1),
                ("D", 0), ("R", 0))
    # UART boot timing is target dependent; never silently assume S31.
    target_from_name(chip)
    enter, release = (1.3, 0.45) if chip == "esp32" else (0.1, 0.05)
    return (("D", 0), ("R", 1), ("W", enter), ("D", 1), ("R", 0),
            ("W", release), ("D", 0))


def execute_reset(serial_port: SerialControlLines, steps: Iterable[Step], *,
                  sleep: Callable[[float], None] = time.sleep) -> None:
    # Validate before touching either output; no evaluated configuration code.
    sequence = tuple(steps)
    if len(sequence) > 32 or any(
        action not in {"D", "R", "W"} or
        (action == "W" and not 0 <= value <= 2) or
        (action != "W" and value not in (0, 1)) for action, value in sequence
    ) or sum(value for action, value in sequence if action == "W") > 5:
        raise ValueError("invalid or unbounded hardware reset sequence")
    try:
        for action, value in sequence:
            if action == "W":
                sleep(value)
            elif action == "D":
                serial_port.dtr = bool(value)
            else:
                serial_port.rts = bool(value)
                # usbser.sys propagates DTR together with RTS updates.
                serial_port.dtr = serial_port.dtr
    except BaseException:
        # A partially completed sequence must not deliberately hold EN low.
        try:
            serial_port.dtr = False
            serial_port.rts = False
        except (OSError, ValueError):
            # The original failure is the useful result if the port vanished.
            pass
        raise
