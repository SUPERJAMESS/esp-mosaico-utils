"""ESP image target identities; recognition does not imply hardware validation.

IDs follow ESP-IDF components/bootloader_support/include/esp_app_format.h.
Keep product contracts and transport capabilities outside this registry.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ChipTarget:
    name: str
    chip_id: int
    architecture: str

    @property
    def gdb(self) -> str:
        return "xtensa-esp-elf-gdb" if self.architecture == "xtensa" else "riscv32-esp-elf-gdb"


CHIP_TARGETS = tuple(
    ChipTarget(name, chip_id, architecture)
    for name, chip_id, architecture in (
        ("esp32", 0x0000, "xtensa"),
        ("esp32s2", 0x0002, "xtensa"),
        ("esp32c3", 0x0005, "riscv"),
        ("esp32s3", 0x0009, "xtensa"),
        ("esp32c2", 0x000C, "riscv"),
        ("esp32c6", 0x000D, "riscv"),
        ("esp32h2", 0x0010, "riscv"),
        ("esp32p4", 0x0012, "riscv"),
        ("esp32c61", 0x0014, "riscv"),
        ("esp32c5", 0x0017, "riscv"),
        ("esp32h21", 0x0019, "riscv"),
        ("esp32h4", 0x001C, "riscv"),
        ("esp32s31", 0x0020, "riscv"),
    )
)
_BY_ID = {target.chip_id: target for target in CHIP_TARGETS}
_BY_NAME = {target.name: target for target in CHIP_TARGETS}


def target_from_chip_id(chip_id: object) -> ChipTarget:
    if type(chip_id) is not int or chip_id not in _BY_ID:
        raise ValueError(f"unsupported or missing firmware chip ID: {chip_id!r}")
    return _BY_ID[chip_id]


def target_from_name(name: object) -> ChipTarget:
    if not isinstance(name, str) or name not in _BY_NAME:
        raise ValueError(f"unsupported or missing chip target: {name!r}")
    return _BY_NAME[name]
