"""Generate reproducible embedded payloads and complete resource images."""
import hashlib
import json
from pathlib import Path
import sys


def generate(sample, destination):
    if sample not in ("A", "A2", "B", "C"):
        raise ValueError("unknown sample")
    destination.mkdir(parents=True, exist_ok=True)
    sizes = {"payload": 0xB40000 if sample == "B" else 4096}
    if sample == "C":
        sizes.update(stress_a=0x100000, stress_b=0x100000)
    records = {}
    for name, size in sizes.items():
        seed = ("mosaico-flash-stress/1/" + sample + "/" + name).encode()
        marker = seed + b"\n" if name != "payload" else b""
        data = marker + hashlib.shake_256(seed).digest(size - len(marker))
        path = destination / (name + ".bin")
        # Keep incremental object timestamps stable when configure is repeated.
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
        records[name] = {"size": size, "sha256": hashlib.sha256(data).hexdigest()}
        if marker:
            records[name]["marker"] = marker.decode().strip()
    header = '#define STRESS_SAMPLE "' + sample + '"\n'
    for name in ("payload", "stress_a", "stress_b"):
        header += '#define STRESS_{}_SHA256 "{}"\n'.format(
            name.upper(), records.get(name, {}).get("sha256", ""))
    path = destination / "stress_inputs.h"
    if not path.exists() or path.read_text() != header:
        path.write_text(header)
    (destination / "inputs.json").write_text(json.dumps(records, indent=2) + "\n")
    return records


if __name__ == "__main__":
    generate(sys.argv[1], Path(sys.argv[2]))
