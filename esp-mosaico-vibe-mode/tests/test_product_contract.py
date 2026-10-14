"""The deployed 64-byte product record remains readable by independent builds."""
import shutil
import struct
import subprocess
from pathlib import Path

import pytest


def test_sysmeta_record_matches_deployed_bytes(tmp_path):
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("host C compiler unavailable")
    source = tmp_path / "contract.c"
    source.write_text('''
#include "mosaico_recovery_contract.h"
#include <stdio.h>
int main(void) {
    mosaico_sysmeta_record_t record = { .magic = MOSAICO_SYSMETA_MAGIC,
        .version = MOSAICO_SYSMETA_VERSION, .result = -7 };
    for (unsigned i = 0; i < sizeof(record.operation_id); ++i) record.operation_id[i] = i;
    const unsigned char *bytes = (const unsigned char *)&record;
    for (unsigned i = 0; i < sizeof(record); ++i) printf("%02x", bytes[i]);
    return 0;
}
''')
    output = tmp_path / "contract"
    include = Path(__file__).resolve().parents[1] / "include"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-I", str(include), str(source), "-o", str(output)], check=True, capture_output=True)
    actual = subprocess.check_output([str(output)], text=True)
    deployed = struct.pack("<II16si36s", 0x49535953, 2, bytes(range(16)), -7, bytes(36))
    assert actual == deployed.hex()
