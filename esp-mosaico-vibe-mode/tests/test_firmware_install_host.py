"""Exercise the production downloader against bounded HTTP and writer fakes."""
import os
from pathlib import Path
import re
import subprocess


def test_firmware_install(tmp_path):
    root = Path(__file__).resolve().parents[1]
    component = root / "firmware/vibe-mode/components/app_update_check"
    http_client = root / "firmware/vibe-mode/components/app_http_client"
    source = component / "src/factory_firmware_install.c"
    fixture = root / "tests/host_firmware"
    for name in re.findall(r'#include "([^"]+)"', source.read_text()) + ["esp_err.h"]:
        if name in {"factory_firmware_install.h", "factory_http_client.h"}:
            continue
        header = tmp_path / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    executable = tmp_path / "install"
    command = ["cc", "-std=gnu11", "-g", "-Werror=implicit-function-declaration",
               '-DFIRMWARE_SOURCE="{}"'.format(source.as_posix()), "-I" + str(tmp_path),
               '-DHTTP_SOURCE="{}"'.format(http_client / "src/factory_http_client.c"),
               "-I" + str(fixture), "-I" + str(component / "include"),
               "-I" + str(http_client / "include"),
               str(fixture / "main.c"), "-lcrypto", "-o", str(executable)]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True, capture_output=True, text=True)
    subprocess.run([str(executable)], check=True, capture_output=True, text=True)
