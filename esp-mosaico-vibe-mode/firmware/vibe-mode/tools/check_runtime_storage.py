#!/usr/bin/env python3
"""Verify that Vibe Mode's USB/runtime storage is internal, startup-cleared BSS."""
import argparse
import json

from elftools.elf.elffile import ELFFile


def check(elf_path, config_path):
    with open(config_path, encoding="utf-8") as source:
        config = json.load(source)
    alignment = (config["CACHE_L1_CACHE_LINE_SIZE"]
                 if config.get("SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE") else 4)
    expected = {"g_iris": 4, "_cdcd_epbuf": alignment,
                "_ctrl_epbuf": alignment, "_dcd_usbbuf": alignment}
    if config.get("ESP_IRIS_DATA_LINK"):
        expected["g_iris_data"] = 4
    with open(elf_path, "rb") as source:
        elf = ELFFile(source)
        symbols = list(elf.get_section_by_name(".symtab").iter_symbols())
        addresses = {symbol.name: symbol["st_value"] for symbol in symbols}
        for name, required_alignment in expected.items():
            matches = [symbol for symbol in symbols if symbol.name == name
                       or symbol.name.startswith(name + ".lto_priv.")]
            if len(matches) != 1:
                raise ValueError("Expected one storage symbol: " + name)
            symbol = matches[0]
            section = elf.get_section(symbol["st_shndx"])
            start, size = symbol["st_value"], symbol["st_size"]
            if (section.name != ".dram0.bss" or section["sh_type"] != "SHT_NOBITS"
                    or size == 0 or start % required_alignment
                    or not addresses["_bss_start"] <= start < start + size <= addresses["_bss_end"]):
                raise ValueError("Storage must be aligned, internal and startup-cleared: " + name)
            print(f"{name}: {size} bytes in internal BSS, alignment {required_alignment}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("sdkconfig")
    args = parser.parse_args()
    check(args.elf, args.sdkconfig)
