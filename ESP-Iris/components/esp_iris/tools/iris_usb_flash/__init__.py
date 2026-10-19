# SPDX-License-Identifier: Apache-2.0
"""IDF action integration; retain native flash targets, arguments and manifests."""
from __future__ import annotations

import atexit
import json
import subprocess
from functools import wraps
from pathlib import Path

import serial

from .ports import UsbFlashError, claim_device, prepare_rom, select_port, wait_for_port


def usb_config(build_dir: str) -> dict:
    path = Path(build_dir) / "config" / "sdkconfig.json"
    if not path.is_file():
        return {}
    config = json.loads(path.read_text(encoding="utf-8"))
    if (config.get("IDF_TARGET") != "esp32s31"
            or not config.get("ESP_IRIS_ENABLE")
            or not config.get("ESP_IRIS_TRANSPORT_USB")
            or not config.get("ESP_IRIS_USB_AUTO_DOWNLOAD")):
        return {}
    return config


def action_extensions(base_actions: dict, project_path: str) -> dict:
    # Change only callbacks, preserving IDF's version-specific options and
    # dependency graph. Both component discovery and a fresh-project shim may
    # load us; the marker prevents double wrapping.
    actions = base_actions.get("actions", {})
    flashed = {"console": None, "claim": None}

    def wrap_flash(original):
        @wraps(original)
        def flash(action, ctx, args, *, reconnect_after_flash=True, **kwargs):
            from idf_py_actions.errors import FatalError
            from idf_py_actions.tools import ensure_build_directory, run_target

            ensure_build_directory(args, ctx.info_name)
            config = usb_config(args.build_dir)
            if not config:
                return original(action, ctx, args, **kwargs)
            if kwargs.get("flash_all") and kwargs.get("trust_flash_content"):
                raise FatalError("--all and --trust-flash-content cannot be combined.")
            claim = None
            try:
                vid = config["ESP_IRIS_USB_VID"]
                pid = config["ESP_IRIS_USB_PID"]
                selected = select_port(args.port, vid, pid)
                if selected is None:
                    return original(action, ctx, args, **kwargs)
                # Compile/check all artifacts before detaching a working device.
                run_target("all", args)
                if select_port(selected.device, vid, pid) != selected:
                    raise UsbFlashError("The selected USB device changed during the build. "
                                        "Select its current console or ROM port and retry.")
                if flashed["claim"] is not None:
                    flashed["claim"].close()
                    flashed["claim"] = None
                flashed["console"] = None
                claim = claim_device(selected)
                rom, mac = prepare_rom(selected, vid, pid)
                args.port = rom.device
                print(f"Iris: verified ROM {mac} at {rom.device}", flush=True)
                result = original(action, ctx, args, **kwargs)
                if not reconnect_after_flash:
                    # A supervising Gateway owns reconnect and live acceptance.
                    # A missing CDC must not turn a completed write into failure.
                    return result
                try:
                    console = wait_for_port(rom, vid, pid, rom=False, expected_mac=mac)
                except UsbFlashError as error:
                    raise UsbFlashError(f"Flash completed, but {error}") from error
                args.port = console.device
                flashed["console"] = console.device
                flashed["claim"] = claim
                # Also release on a later action failure or flash without monitor.
                atexit.register(claim.close)
                claim = None
                print(f"Iris: application console ready at {console.device}", flush=True)
                return result
            except (UsbFlashError, OSError, serial.SerialException,
                    subprocess.SubprocessError) as error:
                raise FatalError(f"Iris USB flash: {error}") from error
            finally:
                if claim is not None:
                    claim.close()
        flash._iris_usb_flash = True
        return flash

    def wrap_monitor(original):
        @wraps(original)
        def monitor(action, ctx, args, **kwargs):
            if flashed["console"] is not None:
                args.port = flashed["console"]
                kwargs["no_reset"] = True
            try:
                return original(action, ctx, args, **kwargs)
            finally:
                if flashed["claim"] is not None:
                    flashed["claim"].close()
                    flashed["claim"] = None
                flashed["console"] = None
        monitor._iris_usb_flash = True
        return monitor

    for name in ("flash", "app-flash", "encrypted-flash", "encrypted-app-flash", "monitor"):
        spec = actions.get(name)
        if not spec or getattr(spec["callback"], "_iris_usb_flash", False):
            continue
        spec["callback"] = (wrap_monitor if name == "monitor" else wrap_flash)(spec["callback"])
    return {}
