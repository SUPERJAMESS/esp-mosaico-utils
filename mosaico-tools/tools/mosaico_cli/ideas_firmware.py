"""Product partition policy applied after the public Iris bundle validator."""
from __future__ import annotations

import hashlib
import re
import struct

from .ideas_content import MIB, read_archive, require, strict_json, version_tuple
from .iris import load_bundle
from .product_contract import PRODUCT_CONTRACT


def validate_firmware(raw):
    members = read_archive(raw, limit=32*MIB, files=16, entries=16, directories=False)
    require("manifest.json" in members, "Missing firmware manifest", code="INVALID_FIRMWARE")
    manifest = strict_json(members["manifest.json"], 3072)
    require("signature" not in manifest and "manifest.sig" not in members,
            "Ideas firmware must be unsigned", code="INVALID_FIRMWARE")
    release = manifest.get("release")
    require(isinstance(release, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}", release),
            "Firmware requires a valid release", code="INVALID_FIRMWARE")
    if "minimum_recovery_version" in manifest:
        version_tuple(manifest["minimum_recovery_version"])
    try:
        bundle = load_bundle(raw)
    except ValueError as error:
        from .ideas_content import ContentError
        raise ContentError("INVALID_FIRMWARE", "Iris rejected the firmware bundle") from error
    require(bundle["chip_id"] == 32 and bundle["flash_size"] <= 32*MIB
            and 2 <= len(bundle["components"]) <= 8, "Unsupported product target", code="INVALID_FIRMWARE")
    components = bundle["components"]
    table = components[0]
    require(table["kind"] == "partition_table" and table["target_offset"] == 0x8000
            and table["size"] == 4096, "Invalid product partition table component", code="INVALID_FIRMWARE")
    require(sum(c["kind"] == "partition_table" for c in components) == 1
            and sum(c["kind"] == "application" for c in components) == 1
            and all(c["kind"] in ("partition_table", "application", "data") and not c["flags"] for c in components),
            "Protected system components are forbidden", code="INVALID_FIRMWARE")
    data = members[table["file"]]
    partitions, spans, checksum = {}, [], False
    for offset in range(0, 3072, 32):
        row = data[offset:offset+32]
        if row == b"\xff"*32:
            require(all(b == 255 for b in data[offset:]), "Trailing partition table bytes", code="INVALID_FIRMWARE")
            break
        if row[:2] == b"\xeb\xeb":
            require(not checksum and row[2:16] == b"\xff"*14
                    and row[16:] == hashlib.md5(data[:offset]).digest(),
                    "Invalid partition table checksum", code="INVALID_FIRMWARE")
            checksum = True
            continue
        require(not checksum, "Partition after checksum", code="INVALID_FIRMWARE")
        magic, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", row)
        require(magic == 0x50aa and size > 0 and start % 4096 == size % 4096 == 0
                and start >= 0x9000 and start+size <= bundle["flash_size"],
                "Invalid partition range", code="INVALID_FIRMWARE")
        try:
            name = label.split(b"\0", 1)[0].decode("ascii")
        except UnicodeError:
            name = ""
        require(name and name not in partitions, "Duplicate/invalid partition label", code="INVALID_FIRMWARE")
        require(not any(start < e and s < start+size for s, e in spans), "Overlapping partitions", code="INVALID_FIRMWARE")
        spans.append((start, start+size))
        partitions[name] = kind, subtype, start, size, flags
    require(checksum, "Missing partition table checksum", code="INVALID_FIRMWARE")
    types = {"app": 0, "data": 1}
    subtypes = {"ota": 0, "phy": 1, "nvs": 2, "coredump": 3, "test": 0x20}
    for name, fields in PRODUCT_CONTRACT["immutable_layout"].items():
        kind, subtype, start, size, flags = fields
        require(partitions.get(name) == (types[kind], subtypes[subtype], start, size, 0),
                "Immutable product partition changed", name, "INVALID_FIRMWARE")
    application = partitions.get("main_app")
    require(application and application[:3] == (0, 0x10, 0x210000) and not application[4],
            "Invalid main_app partition", code="INVALID_FIRMWARE")
    for component in components[1:]:
        found = [p for p in partitions.values() if p[2] == component["target_offset"]]
        require(len(found) == 1 and component["size"] <= found[0][3] and not found[0][4],
                "Component does not fit a writable partition", component["file"], "INVALID_FIRMWARE")
        part = found[0]
        require((component["kind"] == "application" and part == application) or
                (component["kind"] == "data" and part[0] == 1 and part[1] not in (0, 1, 2, 3, 4)
                 and part[2] >= 0x210000), "Component targets protected data", component["file"], "INVALID_FIRMWARE")
    return manifest, members
