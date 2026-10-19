"""Exercise the production lightweight UI and C controller through native input."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

import pytest
import zxingcpp
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
RECOVERY = ROOT / "firmware/vibe-mode"


class Simulator:
    def __init__(self, directory, executable, bundle, host, scenario=""):
        self.directory = directory
        self.state_file = directory / "state.json"
        self.log = (directory / "sim.log").open("w")
        ready = directory / "ready.json"
        self.sim = subprocess.Popen([
            sys.executable, str(host), "--library", str(executable),
            "--directory", str(directory), "--scenario", scenario],
            stdout=self.log, stderr=self.log)
        self.backend = None
        self.connection = None
        self.stream = None
        self.sequence = 0
        try:
            deadline = time.monotonic() + 20
            while not ready.exists():
                assert self.sim.poll() is None, "simulator exited before ready"
                assert time.monotonic() < deadline, "simulator startup timed out"
                time.sleep(.05)
            info = json.loads(ready.read_text())
            self.backend = self.sim
            address, port = info["api"].replace("tcp://", "").split(":")
            self.connection = socket.create_connection((address, int(port)), timeout=15)
            self.stream = self.connection.makefile("rb")
            self.wait(page=0)
        except BaseException:
            self.close()
            raise

    def rpc(self, method, **params):
        self.sequence += 1
        body = json.dumps(dict(jsonrpc="2.0", id=self.sequence, method=method, params=params)).encode()
        self.connection.sendall(("Content-Length: %d\r\n\r\n" % len(body)).encode() + body)
        while True:
            headers = {}
            while True:
                line = self.stream.readline()
                assert line, "simulator disconnected"
                if line in (b"\r\n", b"\n"):
                    break
                key, value = line.decode().split(":", 1)
                headers[key.lower()] = value.strip()
            result = json.loads(self.stream.read(int(headers["content-length"])))
            if result.get("id") == self.sequence:
                assert "error" not in result, result
                return result.get("result")

    def wait(self, *, timeout=15, **expected):
        deadline = time.monotonic() + timeout
        actual = {}
        while time.monotonic() < deadline:
            assert self.backend.poll() is None, (self.directory / "sim.log").read_text()
            try:
                snapshot = self.state_file.read_text(encoding="utf-8")
            except (FileNotFoundError, PermissionError):
                # Windows briefly denies reads while MoveFileEx replaces the
                # snapshot. Retry within the same deadline, never reset it.
                pass
            else:
                actual = json.loads(snapshot)
                if all(actual.get(k) == v for k, v in expected.items()):
                    # The backend timer uses wall time; accelerated renderer
                    # frames alone do not finish its 150ms navigation guard.
                    time.sleep(.2)
                    # Let already-enqueued visibility and list updates paint.
                    self.rpc("wait", frames=20)
                    return actual
            time.sleep(.05)
        pytest.fail("expected %r; observed %r; %s" % (expected, actual, (self.directory / "sim.log").read_text()))

    def tap(self, x, y):
        # Use the same raw input path as the browser/device, including the
        # application pointer observer used for scroll and gesture routing.
        self.rpc("feed_pointer", x=x, y=y, pressed=True)
        self.rpc("wait", frames=2)
        self.rpc("feed_pointer", x=x, y=y, pressed=False)
        self.rpc("wait", frames=8)

    def tap_ui(self, name):
        objects = json.loads((RECOVERY / "ui/main.json").read_text())["objects"]
        obj = next(o for o in objects if o.get("name") == name)
        x, y = obj["x"] + obj["w"] // 2, obj["y"] + obj["h"] // 2
        parent = obj.get("parent", -1)
        while parent >= 0:
            x += objects[parent]["x"]
            y += objects[parent]["y"]
            if objects[parent].get("scroll"):
                offsets = json.loads(self.state_file.read_text())
                y -= offsets[objects[parent]["name"].replace("_viewport", "_offset")]
            parent = objects[parent].get("parent", -1)
        self.tap(x, y)

    def swipe(self, x1, y1, x2, y2):
        # feed_pointer forwards raw samples to the application observer; the
        # simulator's canned drag only exercises widget gesture recognizers.
        self.rpc("feed_pointer", x=x1, y=y1, pressed=True)
        self.rpc("wait", frames=3)
        self.rpc("feed_pointer", x=x2, y=y2, pressed=True)
        self.rpc("wait", frames=3)
        self.rpc("feed_pointer", x=x2, y=y2, pressed=False)

    def manage(self):
        self.tap(240, 360)
        self.wait(page=5)

    def download(self):
        self.manage()
        self.tap_ui("manage_download")

    def wifi(self):
        self.tap(390, 360)
        self.wait(page=11)
        self.tap_ui("button_settings_top_wifi")

    def wifi_back(self):
        self.tap(38, 36)
        self.wait(page=11)
        self.tap(38, 36)

    def capture(self, name):
        self.rpc("screenshot", path=str(self.directory / (name + ".png")), format="png")

    def open_bridge(self):
        # Same public controller entry used by the device's queued USB RPC.
        self.control("open-bridge")

    def control(self, command):
        request = self.directory / "control"
        temporary = self.directory / "control.tmp"
        temporary.write_text(command)
        temporary.replace(request)
        deadline = time.monotonic() + 5
        while request.exists():
            assert time.monotonic() < deadline
            time.sleep(.05)
        self.rpc("wait", frames=20)

    def close(self):
        if self.stream:
            self.stream.close()
        if self.connection:
            self.connection.close()
        # Stop backend first: deinit can still communicate with the renderer.
        for process in (self.backend, self.sim):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        self.log.close()


@pytest.fixture(scope="module")
def native_build(tmp_path_factory):
    build = tmp_path_factory.mktemp("vibe-native-build")
    generator = ["-G", "MinGW Makefiles"] if os.name == "nt" else []
    for command in (["cmake", "-S", str(RECOVERY / "pc"), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                     "-DPython3_EXECUTABLE=" + sys.executable] + generator,
                    ["cmake", "--build", str(build), "--config", "Release", "-j4"]):
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stdout + result.stderr
    library = next((p for name in ("libvibe_ui.so", "libvibe_ui.dylib", "libvibe_ui.dll", "vibe_ui.dll", "Release/vibe_ui.dll")
                    if (p := build / name).is_file()), None)
    assert library is not None, "Native UI library was not produced"
    return library, None, RECOVERY / "pc/simulator.py"



@pytest.fixture
def sim(tmp_path, native_build, request):
    instance = Simulator(tmp_path, *native_build, scenario=getattr(request, "param", ""))
    try:
        yield instance
    finally:
        instance.close()


def test_home_maker_animation_captures_gestures(sim):
    sim.capture("maker-start")
    sim.rpc("wait", frames=180)
    sim.capture("maker-later")
    with Image.open(sim.directory / "maker-start.png") as a, Image.open(sim.directory / "maker-later.png") as b:
        changed = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).getbbox()
        assert changed and 280 <= changed[0] < changed[2] <= 452
        assert 64 <= changed[1] < changed[3] <= 236
    sim.swipe(400, 120, 90, 120)
    sim.wait(page=0, install_calls=0)
    sim.swipe(420, 250, 60, 250)
    sim.wait(page=19)
    sim.swipe(90, 250, 400, 250)
    sim.wait(page=0)
    sim.tap_ui("button_nav_settings"); sim.wait(page=11)
    sim.capture("settings-still")
    sim.rpc("wait", frames=100)
    sim.capture("settings-later")
    assert (sim.directory / "settings-still.png").read_bytes() == (sim.directory / "settings-later.png").read_bytes()


@pytest.mark.parametrize("sim", ["prefetched"], indirect=True)
@pytest.mark.parametrize("language", [1, 2])
def test_release_notice_and_manual_recheck(sim, language):
    if language == 2:
        sim.tap(390, 360); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.control("ideas-refresh-busy")
    sim.wifi()
    sim.wait(page=1)
    sim.control("release-available")
    sim.wait(page=1)
    sim.wifi_back()
    sim.wait(page=26)
    time.sleep(.5)
    sim.capture("release-available")
    with Image.open(sim.directory / "release-available.png") as frame:
        assert frame.convert("RGB").crop((180, 152, 300, 192)).getbbox(), "release version must be visible"
    sim.tap_ui("button_firmware_later")
    sim.wait(page=0)
    time.sleep(1)
    sim.wait(page=0)
    sim.tap(390, 360); sim.wait(page=11)
    sim.swipe(240, 410, 240, 170); sim.wait(page=11)
    sim.tap_ui("button_settings_bottom_about"); sim.wait(page=16)
    sim.tap_ui("button_about_check")
    sim.wait(page=26)


@pytest.mark.parametrize("language", [1, 2])
def test_firmware_install_warns_and_blocks_navigation(sim, language):
    if language == 2:
        sim.tap(390, 360); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.control("ideas-refresh-busy")
    sim.control("release-available")
    sim.wait(page=26)
    # A refreshed feed must install the version currently displayed.
    sim.control("release-available 0.2.2")
    sim.wait(page=26)
    sim.tap_ui("button_firmware_start")
    sim.wait(page=27, progress=250)
    time.sleep(.5)
    sim.capture("firmware-install")
    with Image.open(sim.directory / "firmware-install.png") as frame:
        assert frame.convert("RGB").crop((24, 268, 456, 308)).getbbox(), "power warning must be visible"
    sim.tap(38, 36)
    sim.control("ai")
    sim.wait(page=27, asleep=False)
    sim.control("firmware-complete")
    sim.wait(page=27, progress=1000)
    sim.control("firmware-fail")
    sim.wait(page=27)
    sim.tap_ui("button_firmware_failure_back")
    sim.wait(page=0)


def test_keyboard_limits_masking_and_cancel(sim):
    sim.capture("home")
    sim.download()
    sim.wait(page=1, pending=True)
    sim.capture("wifi")
    sim.tap(180, 251)
    sim.wait(page=2, password_length=0, password_visible=False)
    sim.capture("keyboard")
    for _ in range(65):
        sim.tap(35, 258)  # native q key; the 65th character must be rejected
    sim.wait(password_length=64)
    sim.capture("password-masked")
    sim.tap(431, 376)  # Backspace
    sim.wait(password_length=63)
    sim.tap(414, 146)
    sim.wait(password_visible=True)
    sim.tap(38, 36)
    sim.wait(page=1, password_length=0, password_visible=False, pending=True)
    sim.wifi_back()
    sim.wait(page=0, pending=False, bridge_running=False)


def test_download_wifi_continuation_and_cancel(sim):
    sim.download()
    sim.wait(page=1, pending=True)
    sim.tap(180, 251)
    sim.wait(page=2)
    for _ in range(8):
        sim.tap(35, 258)
    sim.wait(password_length=8)
    sim.tap(420, 435)  # GO
    sim.wait(page=4, pending=False, network=2, bridge_running=False, password_length=0)
    sim.capture("download-ideas")
    sim.wait(bridge_open_calls=0, bridge_prefetch_calls=0)
    sim.open_bridge()
    sim.open_bridge()
    sim.wait(page=4, bridge_running=False, bridge_open_calls=0)
    sim.tap(38, 36)
    sim.wait(page=0, bridge_running=False)
    sim.wifi()  # ordinary Wi-Fi entry never starts a download
    sim.wait(page=1, pending=False, bridge_running=False)


@pytest.mark.parametrize("sim", ["prefetched"], indirect=True)
def test_six_digit_code_download_never_installs_or_pairs(sim):
    sim.wait(page=0, bridge_prefetch_calls=0, bridge_open_calls=0, bridge_running=False)
    sim.download()
    sim.wait(page=4, idea_code="")
    sim.tap_ui("button_download_code")  # incomplete code must not issue a cloud request
    sim.wait(page=4, download_calls=0)
    for digit in (0, 4, 2, 7, 3, 1, 9):  # seventh digit is ignored
        sim.tap_ui("button_code_" + str(digit))
    sim.wait(idea_code="042731", download_calls=0)
    sim.tap_ui("button_code_delete")  # Delete
    sim.wait(idea_code="04273")
    sim.tap_ui("button_code_1")
    sim.capture("six-digit-download")
    sim.tap_ui("button_download_code")
    sim.wait(page=9, idea_ready=True, prepare_calls=1, download_calls=0, last_code="042731", install_calls=0)
    sim.capture("download-confirmation")
    sim.tap(120, 436)  # Cancel preserves input and never downloads.
    sim.wait(page=4, idea_code="042731", cancel_calls=1, download_calls=0)
    sim.tap_ui("button_download_code")
    sim.wait(page=9, idea_ready=True, prepare_calls=2, download_calls=0)
    # Deliver duplicate clicks before advancing the UI clock; a delayed
    # second click would intentionally activate the download's Cancel button.
    sim.rpc("feed_pointers", samples=[
        dict(x=350, y=436, pressed=pressed) for pressed in (True, False, True, False)
    ])
    sim.wait(download_calls=1, last_code="042731", install_calls=0)
    sim.wait(timeout=45, page=8, progress=1000, boot_first=False, restart_calls=0)
    sim.wait(bridge_open_calls=0, bridge_prefetch_calls=0, bridge_running=False)
    sim.capture("download-result")
    with Image.open(sim.directory / "download-result.png") as screenshot:
        for box in ((160, 88, 456, 176), (24, 380, 232, 464)):
            pixels = screenshot.convert("RGB").crop(box)
            assert sum(min(pixel) > 220 for pixel in pixels.getdata()) > 100, \
                "result heading and return button must retain readable text"


def test_wifi_cards_and_labels_survive_navigation_and_rescan(sim):
    def check_frame(name):
        sim.capture(name)
        path = sim.directory / (name + ".ppm")
        sim.rpc("screenshot", path=str(path), format="ppm")
        with path.open("rb") as frame:
            assert frame.readline().strip() == b"P6"
            assert frame.readline().split() == [b"480", b"480"]
            assert frame.readline().strip() == b"255"
            pixels = frame.read()
        assert len(pixels) == 480 * 480 * 3

        def region(x1, y1, x2, y2):
            return [pixels[(y * 480 + x) * 3:(y * 480 + x) * 3 + 3]
                    for y in range(y1, y2) for x in range(x1, x2)]

        # Actual rendered glyphs, not just successful visibility callbacks.
        for box, minimum in [((88, 30, 220, 67), 140),  # Reference 28px header
                             ((20, 20, 50, 50), 5),    # Back
                             ((360, 102, 438, 145), 140),  # Forget
                             ((420, 32, 452, 64), 140)]:  # Rescan
            assert sum(max(rgb) > 160 for rgb in region(*box)) > minimum, name
        # Three complete cards, separated from each other and the footer.
        for y in (306, 394):
            assert all(max(rgb) < 20 for rgb in region(48, y, 430, y + 2)), name
        for y in (296, 384, 432):
            assert all(min(rgb) >= 15 for rgb in region(80, y, 380, y + 2)), name

    for cycle in range(3):
        sim.wifi()
        sim.wait(page=1)
        check_frame("wifi-entry-%d" % cycle)
        sim.tap(180, 251)
        sim.wait(page=2)
        sim.tap(38, 36)
        sim.wait(page=1)
        sim.tap_ui("button_scan")
        sim.wait(page=1)
        check_frame("wifi-rescan-%d" % cycle)
        sim.wifi_back()
        sim.wait(page=0)


def test_nand_update_and_result_acknowledgement(sim):
    sim.manage()
    sim.wait(page=5)
    sim.capture("nand")
    sim.tap(150, 170)
    sim.wait(page=6)
    sim.capture("confirm")
    sim.tap_ui("button_local_open")
    sim.wait(page=20)
    sim.tap(350, 396)
    sim.wait(page=7)
    sim.capture("update")
    # Twenty asynchronous service ticks each issue renderer RPCs. macOS CI
    # can exceed 15 wall-clock seconds while still advancing normally.
    sim.wait(timeout=45, page=8, progress=1000, boot_first=True, restart_calls=1, install_calls=1)
    sim.capture("result")
    sim.tap_ui("button_result_library")
    sim.wait(page=5)
    sim.tap(38, 36)
    sim.wait(page=0)
    time.sleep(.6)
    sim.wait(page=0)  # an acknowledged terminal result must not reopen


@pytest.mark.parametrize("sim", ["wifi-fail-once"], indirect=True)
def test_network_failure_and_retry_preserve_download_intent(sim):
    sim.download()
    sim.wait(page=1, pending=True)
    for expected_network in (3, 2):
        sim.tap(180, 251)
        sim.wait(page=2, password_length=0)
        for _ in range(8):
            sim.tap(35, 258)
        sim.tap(420, 435)
        if expected_network == 3:
            sim.wait(page=1, network=3, pending=True, password_length=0)
            sim.capture("network-failure")
        else:
            sim.wait(page=4, network=2, pending=False, bridge_running=False)


@pytest.mark.parametrize("sim", ["update-fail"], indirect=True)
def test_update_failure_returns_home_without_reopening(sim):
    sim.manage()
    sim.wait(page=5)
    sim.tap(150, 170)
    sim.wait(page=6)
    sim.tap_ui("button_local_open")
    sim.wait(page=20)
    sim.tap(350, 396)
    sim.wait(page=8, progress=500)
    sim.capture("update-failure")
    sim.tap_ui("button_result_library")
    sim.wait(page=5)
    sim.tap(38, 36)
    sim.wait(page=0)
    time.sleep(.6)
    sim.wait(page=0)


@pytest.mark.parametrize("sim", ["empty"], indirect=True)
def test_empty_lists_remain_navigable(sim):
    sim.wifi()
    sim.wait(page=1, pending=False)
    sim.capture("wifi-empty")
    sim.tap(180, 251)
    sim.wait(page=1)
    sim.wifi_back()
    sim.wait(page=0)
    sim.manage()
    sim.wait(page=5)
    sim.capture("nand-empty")
    sim.tap(150, 170)
    sim.wait(page=5)
    sim.tap(38, 36)
    sim.wait(page=0)


@pytest.mark.parametrize("sim", ["prefetched"], indirect=True)
def test_check_updates_uses_selected_id_and_does_not_install(sim):
    sim.manage()
    sim.wait(page=5)
    sim.tap(150, 170)
    sim.wait(page=6)
    sim.tap_ui("button_check_idea")
    sim.wait(page=9, idea_ready=True, check_calls=1, last_code="042731", download_calls=0, install_calls=0, boot_first=False)
    sim.tap(120, 436)
    sim.wait(page=6, download_calls=0, install_calls=0)


def test_ota_telemetry_component_transition_stall_retry_and_terminal_states(sim):
    def sample(ms, received, total=4*1024*1024, component=1, completed=0,
               receiving=1, job=1, component_received=None, terminal=0):
        if component_received is None:
            component_received = received
        sim.control("transfer %d %d %d %d 2 %d %d %d %d 2097152 %d" %
                    (ms, received, total, component, completed, receiving, job, component_received, terminal))
        return sim.wait(sample_ms=ms)

    sample(0, 0)
    sim.wait(page=7, progress=0, rate_valid=False)
    sample(1000, 1024*1024)
    sim.wait(progress=250, rate_valid=True, rate_bps=1024*1024)
    sim.capture("ota-downloading")
    sample(2000, 1024*1024)
    sim.wait(progress=250, rate_bps=0)
    sim.capture("ota-stalled")
    sample(2250, 2*1024*1024, receiving=0)
    sim.wait(progress=500, rate_valid=False)
    sim.capture("ota-component-verified")
    sample(2500, 2*1024*1024, component=2, completed=1, component_received=0)
    sim.wait(progress=500, rate_valid=False, component_id=2)
    sample(3500, 3*1024*1024, component=2, completed=1, component_received=1024*1024)
    sim.wait(progress=750, rate_valid=True, rate_bps=1024*1024)
    sim.capture("ota-second-component")
    sample(3750, 2*1024*1024, component=2, completed=1, component_received=0)
    sim.wait(progress=500, rate_valid=False)  # retry rolls back only the active payload
    sample(4000, 0, total=0, job=2)
    sim.wait(progress=0, rate_valid=False)
    sim.capture("ota-unknown-size")
    sample(5000, 1024, total=0, job=2)
    sim.wait(rate_valid=True, rate_bps=1024)
    sample(6000, 4*1024*1024, component=0, completed=2, receiving=0, job=3)
    sim.wait(page=7, progress=1000, rate_valid=False)
    sim.capture("ota-committing")
    sample(7000, 4*1024*1024, component=0, completed=2, receiving=0, job=3, terminal=2)
    sim.wait(page=8, progress=1000, rate_valid=False)
    sim.capture("ota-failed-after-transfer")
    sim.tap_ui("button_result_library")
    sim.wait(page=5)
    sim.tap(38, 36)
    sim.wait(page=0)


@pytest.mark.parametrize("sim", ["slow-preview"], indirect=True)
def test_cancel_loading_discards_late_preview(sim):
    sim.open_bridge()
    for _ in range(6):
        sim.tap_ui("button_code_0")
    sim.tap_ui("button_download_code")
    sim.wait(page=9, idea_loading=True, download_calls=0)
    sim.tap(120, 436)
    sim.wait(page=4, cancel_calls=1, download_calls=0)
    time.sleep(8)
    sim.wait(page=4, idea_ready=False, download_calls=0)


@pytest.mark.parametrize("sim", ["already-downloaded"], indirect=True)
def test_complete_release_opens_library_without_downloading(sim):
    sim.open_bridge()
    for _ in range(6):
        sim.tap_ui("button_code_0")
    sim.tap_ui("button_download_code")
    sim.wait(page=9, idea_ready=True, download_calls=0)
    sim.tap(350, 436)
    sim.wait(page=5, download_calls=0, install_calls=0)
    sim.tap(150, 170)
    sim.wait(page=6)
    sim.tap_ui("button_check_idea")
    sim.wait(page=8, check_calls=1, download_calls=0)


@pytest.mark.parametrize("sim", ["preview-fail"], indirect=True)
def test_preview_failure_never_starts_download(sim):
    sim.open_bridge()
    for _ in range(6):
        sim.tap_ui("button_code_0")
    sim.tap_ui("button_download_code")
    sim.wait(page=8, idea_ready=False, download_calls=0, install_calls=0)
    sim.tap_ui("button_result_library")
    sim.wait(page=5)
    sim.tap(38, 36)
    sim.wait(page=0)


@pytest.mark.parametrize("sim", ["custom-preview"], indirect=True)
def test_custom_font_pixels_and_offline_view_share_native_controls(sim):
    sim.manage(); sim.wait(page=5)
    sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui("button_preview_idea")  # offline information
    sim.wait(page=9, idea_ready=True, download_calls=0)
    sim.capture("custom-offline")
    with Image.open(sim.directory / "custom-offline.png") as image:
        r, g, b = image.convert("RGB").getpixel((100, 40))
        assert r > 200 and g < 20 and b < 20
        r, g, b = image.convert("RGB").getpixel((40, 245))
        assert g > 200 and r < 20 and b < 20
    sim.swipe(220, 345, 220, 250)
    sim.wait(page=9, idea_description_offset=40, download_calls=0, install_calls=0)
    sim.capture("custom-offline-scrolled")
    with Image.open(sim.directory / "custom-offline.png") as before, Image.open(sim.directory / "custom-offline-scrolled.png") as after:
        # Only the description moves; the native title pixels, metadata and
        # action buttons retain their positions and remain unobscured.
        for bounds in ((0, 0, 480, 228), (0, 380, 480, 480)):
            assert before.crop(bounds).tobytes() == after.crop(bounds).tobytes()
    sim.tap(350, 436)  # no download from read-only view
    sim.wait(page=9, download_calls=0, install_calls=0)
    sim.tap(120, 436); sim.wait(page=6)
    sim.tap_ui("button_check_idea"); sim.wait(page=9, idea_ready=True, check_calls=1, idea_description_offset=0)
    sim.tap(350, 436)
    sim.wait(timeout=45, page=8, download_calls=1, install_calls=0)


@pytest.mark.parametrize("sim", ["custom-preview-fail"], indirect=True)
def test_display_adapter_failure_cancels_without_download(sim):
    sim.open_bridge()
    for _ in range(6): sim.tap_ui("button_code_0")
    sim.tap_ui("button_download_code")
    sim.wait(page=8, cancel_calls=1, idea_ready=False, download_calls=0)


def test_system_language_switch_is_offline_and_reversible(sim):
    sim.wait(page=0, language=1, network=0)
    sim.tap_ui("button_nav_settings"); sim.wait(page=11)
    sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
    sim.capture("system-en")
    sim.tap_ui("button_language_zh"); sim.wait(page=13, language=2, network=0, download_calls=0)
    sim.capture("system-zh")
    en = Image.open(sim.directory / "system-en.png").crop((80, 24, 400, 60))
    zh = Image.open(sim.directory / "system-zh.png").crop((80, 24, 400, 60))
    assert zh.tobytes() != en.tobytes()
    sim.tap_ui("button_language_en"); sim.wait(page=13, language=1, network=0, download_calls=0)
    sim.capture("system-en-again")
    again = Image.open(sim.directory / "system-en-again.png").crop((80, 24, 400, 60))
    assert en.tobytes() == again.tobytes()


@pytest.mark.parametrize("english", [False, True])
def test_ideas_download_verification_stages(sim, english):
    if not english:
        sim.tap_ui("button_nav_settings"); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
    sim.control("ideas-phase 0 0 1000000 0 12 0")
    sim.wait(page=7, progress=0, rate_valid=False)
    sim.control("ideas-phase 0 500000 1000000 0 12 1000")
    sim.wait(page=7, progress=500, rate_valid=True, rate_bps=500000)
    sim.capture("ideas-transfer")
    sim.control("ideas-phase 1 0 1000000 0 12 2000")
    sim.wait(page=7, progress=0, rate_valid=False)
    sim.control("ideas-phase 1 400000 1000000 4 12 3000")
    sim.wait(page=7, progress=400, rate_valid=False)
    sim.capture("ideas-verify-files")
    with Image.open(sim.directory / "ideas-verify-files.png") as frame:
        assert frame.getpixel((100, 325))[0] > 200
        assert frame.getpixel((300, 325))[0] < 100
    for phase, name in ((2, "check-image"), (3, "save"), (4, "complete")):
        sim.control("ideas-phase %d 1000000 1000000 12 12 %d" % (phase, 3000 + phase*1000))
        sim.wait(page=8 if phase == 4 else 7, rate_valid=False)
        sim.capture("ideas-" + name)
        if phase in (2, 3):
            with Image.open(sim.directory / ("ideas-" + name + ".png")) as frame:
                pixels = list(frame.convert("RGB").crop((24, 324, 456, 328)).getdata())
                orange = sum(r > 200 and g < 120 for r, g, b in pixels)
                assert 300 < orange < 600  # Activity segment, no fake percentage.


def test_ideas_verification_failure_has_no_download_rate(sim):
    sim.control("ideas-phase 0 500000 1000000 0 12 0")
    sim.control("ideas-phase 0 1000000 1000000 0 12 1000")
    sim.wait(page=7, rate_valid=True)
    sim.control("ideas-phase 1 400000 1000000 4 12 2000")
    sim.wait(page=7, progress=400, rate_valid=False)
    sim.control("ideas-fail")
    sim.wait(page=8, rate_valid=False)
    sim.capture("ideas-verification-failed")
    with Image.open(sim.directory / "ideas-verification-failed.png") as frame:
        pixels = frame.convert("RGB").crop((32, 76, 152, 196)).getdata()
        assert sum(r > 100 and r > g * 1.4 for r, g, b in pixels) > 100
    sim.control("library-refreshed")
    sim.tap(430, 420)  # Wide Done replaces Open, including its former hit region.
    sim.wait(page=5, restart_calls=0, install_calls=0)

def test_manage_reuses_library_when_reentered(sim):
    for _ in range(3):
        sim.manage()
        sim.wait(page=5, bundle_count=2, nand_scanning=False, scan_calls=0)
        sim.tap(38, 36)
        sim.wait(page=0)


@pytest.mark.parametrize("sim", ["open-check"], indirect=True)
@pytest.mark.parametrize("language", [1, 2])
def test_open_read_rate_and_cancel_returns_home(sim, language):
    if language == 2:
        sim.tap(390, 360); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.swipe(420, 250, 60, 250)
    sim.wait(page=19)
    sim.tap(92, 150)
    sim.wait(page=20)
    sim.tap_ui("button_open_accept")
    sim.wait(page=7, cancellable=True)
    sim.control("open-read 0 1000000 0 12 0")
    sim.wait(rate_valid=False)
    sim.control("open-read 500000 1000000 4 12 1000")
    sim.wait(page=7, progress=500, rate_valid=True, rate_bps=500000)
    sim.capture("open-read-rate")
    sim.tap_ui("button_cancel_download")
    sim.wait(page=7, cancellable=False, cancel_calls=1)
    sim.capture("open-cancelling")
    sim.wait(page=8, cancelled=True, restart_calls=0)
    sim.capture("open-cancelled")
    sim.control("library-refreshed")  # Background work may replace service flags.
    assert_wide_done(sim, "cancelled-wide-done")
    sim.wait(page=0, restart_calls=0, scan_calls=0)
    sim.manage()
    sim.wait(page=5, bundle_count=2, scan_calls=0)


def test_home_grid_swipe_and_current_idea(sim):
    sim.swipe(420, 250, 60, 250)
    sim.wait(page=19, grid_page=0)
    sim.capture("idea-grid")
    with Image.open(sim.directory / "idea-grid.png") as frame:
        assert max(frame.getpixel((22, 160))[:3]) < 20  # first Idea is not current
        assert frame.getpixel((183, 126))[0] > 200  # only the second Idea has the current border
        assert max(frame.getpixel((388, 320))[:3]) < 20  # no placeholder icon/card in empty cells
    sim.tap(390, 320)  # empty cell after the add tile has no action
    sim.wait(page=19)
    sim.tap(92, 150)
    sim.wait(page=20, install_calls=0)
    sim.capture("open-idea")
    sim.tap(100, 390)
    sim.wait(page=19, install_calls=0)
    sim.swipe(60, 250, 420, 250)
    sim.wait(page=0)


@pytest.mark.parametrize("sim", ["unsaved"], indirect=True)
def test_unsaved_development_has_only_open_or_cancel(sim):
    sim.manage(); sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui("button_local_open"); sim.wait(page=20)
    sim.capture("unsaved-development")
    sim.tap_ui("button_toggle_open_confirm")  # Hidden preference cannot bypass the warning.
    sim.wait(page=20, open_confirm=True, install_calls=0)
    sim.control("ai")  # hardware cancel preserves development and NAND
    sim.wait(page=6, install_calls=0, save_calls=0)
    sim.tap_ui("button_local_open"); sim.wait(page=20)
    sim.tap(350, 396)
    sim.wait(timeout=45, page=8, restart_calls=1, install_calls=1, save_calls=0)


@pytest.mark.parametrize("sim", ["batch"], indirect=True)
def test_batch_update_survives_navigation_and_can_cancel(sim):
    sim.manage()
    sim.tap_ui("manage_update_all")
    sim.wait(page=31, batch=False, check_calls=0)
    sim.tap_ui("button_update_all_accept")
    sim.wait(page=5, batch=True, check_calls=1, install_calls=0)
    sim.rpc("wait", frames=120)
    sim.capture("batch-update")
    with Image.open(sim.directory / "batch-update.png") as frame:
        # The second row has changed from its long version/current metadata to
        # "Waiting"; a cached scroll image must not keep drawing the old text.
        assert all(max(rgb[:3]) < 80 for rgb in frame.crop((250, 282, 380, 304)).getdata())
    sim.tap(38, 36)
    sim.wait(page=0, batch=True)
    sim.manage()
    sim.wait(page=5, batch=True)
    sim.tap_ui("manage_update_all")
    sim.wait(page=5, batch=False, cancel_calls=1, install_calls=0, restart_calls=0)
    sim.rpc("wait", frames=60)  # Stay in the library after the next status poll.
    sim.wait(page=5, batch=False)


@pytest.mark.parametrize("sim", ["low-battery"], indirect=True)
def test_low_battery_blocks_install_then_allows_after_charging(sim):
    sim.manage(); sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui("button_local_open"); sim.wait(page=20)
    sim.tap(350, 396)
    sim.wait(page=20, install_calls=0, restart_calls=0)
    sim.capture("low-battery")
    sim.control("battery 60")
    sim.tap(350, 396)
    sim.wait(timeout=45, page=8, restart_calls=1, install_calls=1)


@pytest.mark.parametrize("sim", ["development"], indirect=True)
@pytest.mark.parametrize("language", [1, 2])
def test_develop_cards_guide_and_save_use_live_services(sim, language):
    if language == 2:
        sim.tap_ui("button_nav_settings"); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.tap_ui("button_nav_develop"); sim.wait(page=10)
    cards = []
    for name, connected, owner in (("disconnected", 0, 0), ("waiting", 1, 0), ("ready", 1, 1)):
        sim.control("usb %d %d" % (connected, owner))
        sim.wait(page=10, save_calls=0, restart_calls=0)
        sim.capture("develop-" + name)
        with Image.open(sim.directory / ("develop-" + name + ".png")) as frame:
            cards.append(frame.crop((24, 96, 456, 218)).tobytes())
    assert len(set(cards)) == 3
    sim.tap_ui("button_develop_guide"); sim.wait(page=25)
    sim.capture("develop-guide")
    with Image.open(sim.directory / "develop-guide.png") as frame:
        assert [code.text for code in zxingcpp.read_barcodes(frame)] == ["https://mosaico.espressif.com/guide/vibe-coding/"]
    sim.tap(38, 36); sim.wait(page=10)
    sim.tap_ui("button_develop_save")
    sim.wait(timeout=45, page=8, save_calls=1, development_saved=True, restart_calls=0)


def hold(sim, x=240, y=396, **expected):
    sim.rpc("feed_pointer", x=x, y=y, pressed=True)
    try:
        sim.wait(timeout=25, **expected)
    finally:
        sim.rpc("feed_pointer", x=x, y=y, pressed=False)


def test_delete_requires_hold_and_supports_undo(sim):
    sim.manage(); sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui("button_local_delete"); sim.wait(page=23)
    sim.tap(240, 396); sim.wait(page=23, delete_calls=0)
    hold(sim, page=5, pending_delete=True, delete_calls=0)
    sim.capture("undo-idea")
    sim.tap(390, 435)
    sim.wait(page=5, pending_delete=False, delete_calls=0, bundle_count=2)
    sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui("button_local_delete"); sim.wait(page=23)
    hold(sim, page=5, pending_delete=True)
    sim.tap(38, 36)  # leaving commits the staged deletion
    sim.wait(page=0, pending_delete=False, delete_calls=1, bundle_count=1)


def test_language_display_and_ai_does_not_sleep(sim):
    sim.tap(390, 360); sim.wait(page=11)
    sim.tap_ui("button_settings_top_display"); sim.wait(page=12)
    sim.tap(145, 155); sim.wait(brightness=40)
    sim.tap(420, 275); sim.wait(screen_off_seconds=0)
    sim.tap(125, 360); sim.wait(rotate=False)
    sim.swipe(240, 410, 240, 180)
    sim.wait(display_offset=40)
    sim.tap_ui("button_display_0_haptics"); sim.wait(haptics=False)
    sim.tap(38, 36); sim.wait(page=11)
    sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
    sim.tap_ui("button_language_zh"); sim.wait(language=2)
    sim.capture("chinese-language")
    sim.tap(38, 36); sim.wait(page=11)
    sim.capture("chinese-settings")
    sim.tap(38, 36); sim.wait(page=0)
    sim.control("ai")
    time.sleep(.3)  # Let the queued hardware key reach the UI tick.
    sim.wait(page=0, asleep=False)
    sim.swipe(400, 250, 80, 250); sim.wait(page=19)
    sim.control("ai")
    time.sleep(.3)
    sim.wait(page=19, asleep=False)


@pytest.mark.parametrize("sim", ["first-run"], indirect=True)
def test_first_run_defaults_to_english(sim):
    sim.wait(page=0, language=1)
    sim.capture("first-run")
    sim.tap_ui("button_nav_settings"); sim.wait(page=11)
    sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
    sim.tap_ui("button_language_zh"); sim.wait(language=2)


@pytest.mark.parametrize("language", [1, 2])
def test_factory_reset_requires_hold_and_preserves_nand(sim, language):
    if language == 2:
        sim.tap(390, 360); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.tap(390, 360); sim.wait(page=11)
    sim.swipe(240, 410, 240, 100)  # Reach the end without relying on inertia.
    sim.wait(settings_offset=280)
    sim.tap_ui("button_settings_bottom_reset"); sim.wait(page=17)
    sim.capture("factory-reset")
    with Image.open(sim.directory / "factory-reset.png") as frame:
        idle = frame.copy()
        assert all(max(rgb[:3]) < 20 for rgb in idle.crop((24, 300, 456, 340)).getdata())  # No separate countdown.
    sim.tap(240, 396); sim.wait(page=17, reset_calls=0)
    sim.rpc("feed_pointer", x=240, y=396, pressed=True)
    time.sleep(1)
    sim.capture("factory-reset-holding")
    with Image.open(sim.directory / "factory-reset-holding.png") as frame:
        assert frame.getpixel((80, 370)) != idle.getpixel((80, 370))
        # The unfilled end receives only the 4% press highlight.
        assert max(abs(a-b) for a, b in zip(frame.getpixel((430, 370)), idle.getpixel((430, 370)))) <= 12
        assert not ImageChops.difference(frame, idle).crop((0, 0, 480, 348)).getbbox()
    sim.rpc("feed_pointer", x=240, y=396, pressed=False)
    sim.wait(page=17, reset_calls=0)
    sim.capture("factory-reset-released")
    with Image.open(sim.directory / "factory-reset-released.png") as frame:
        assert not ImageChops.difference(frame, idle).getbbox()
    # Leaving the hit area cancels this hold without releasing the pointer.
    sim.rpc("feed_pointer", x=240, y=396, pressed=True)
    time.sleep(.8)
    sim.rpc("feed_pointer", x=240, y=300, pressed=True)
    sim.capture("factory-reset-outside")
    with Image.open(sim.directory / "factory-reset-outside.png") as frame:
        assert not ImageChops.difference(frame, idle).getbbox()
    sim.rpc("feed_pointer", x=240, y=300, pressed=False)
    hold(sim, reset_calls=1, restart_calls=1, bundle_count=2, shared_count=0, language=1)


@pytest.mark.parametrize("language", [1, 2])
def test_status_bar_brand_and_version_updates(sim, language):
    if language == 2:
        sim.tap(390, 360); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.control("firmware 0.2.7")
    sim.capture("home-version")
    with Image.open(sim.directory / "home-version.png") as frame:
        header = frame.crop((0, 0, 480, 56))
        assert frame.crop((128, 20, 192, 44)).getbbox()  # Version follows the brand.
    sim.swipe(420, 250, 60, 250)
    sim.wait(page=19)
    sim.capture("grid-version")
    with Image.open(sim.directory / "grid-version.png") as frame:
        assert not ImageChops.difference(header, frame.crop((0, 0, 480, 56))).crop((30, 20, 238, 44)).getbbox()
        grid_header = frame.crop((0, 0, 480, 56))
    sim.control("firmware 0.2.7-12345678901234567890-dirty")
    sim.capture("grid-version-long")
    with Image.open(sim.directory / "grid-version-long.png") as frame:
        changed = ImageChops.difference(grid_header, frame.crop((0, 0, 480, 56)))
        assert changed.crop((30, 20, 238, 44)).getbbox()
        assert not changed.crop((30, 20, 156, 44)).getbbox()  # Brand and version prefix stay readable.
        assert not changed.crop((238, 0, 480, 56)).getbbox()  # No overlap with title/status icons.


def test_reset_hold_cancels_on_navigation_and_runs_once(sim):
    def enter_reset():
        sim.tap(390, 360); sim.wait(page=11)
        sim.swipe(240, 410, 240, 100); sim.wait(settings_offset=280)
        sim.tap_ui("button_settings_bottom_reset"); sim.wait(page=17)

    enter_reset()
    sim.rpc("feed_pointer", x=240, y=396, pressed=True)
    time.sleep(.8)
    sim.control("ai")
    sim.wait(page=0, reset_calls=0)
    sim.rpc("feed_pointer", x=240, y=396, pressed=False)
    enter_reset()
    sim.capture("reset-reentered")
    with Image.open(sim.directory / "reset-reentered.png") as frame:
        assert frame.getpixel((80, 370)) == frame.getpixel((430, 370))
    # A gesture that begins outside the reset button must never arm it.
    sim.rpc("feed_pointer", x=240, y=300, pressed=True)
    sim.rpc("feed_pointer", x=240, y=396, pressed=True)
    time.sleep(3.2)
    sim.wait(page=17, reset_calls=0)
    sim.rpc("feed_pointer", x=240, y=396, pressed=False)
    sim.rpc("feed_pointer", x=240, y=396, pressed=True)
    try:
        sim.wait(reset_calls=1)
        sim.rpc("feed_pointer", x=241, y=396, pressed=True)
        time.sleep(3.2)
        sim.wait(reset_calls=1, restart_calls=1)
    finally:
        sim.rpc("feed_pointer", x=241, y=396, pressed=False)


def test_shared_values_save_and_delete_undo(sim):
    sim.tap(390, 360); sim.wait(page=11)
    sim.tap_ui("button_settings_top_shared"); sim.wait(page=14)
    sim.tap_ui("shared_header_add"); sim.wait(page=15)
    sim.tap(35, 250)  # q as key
    sim.tap(420, 430)  # Next
    sim.tap(35, 250)  # q as value
    sim.tap(420, 430)  # Save
    sim.wait(page=14, shared_count=1)
    sim.capture("shared-values")
    sim.tap(240, 122); sim.wait(page=30)
    sim.tap_ui("button_shared_detail_delete"); sim.wait(page=23)
    hold(sim, page=14, pending_delete=True, shared_count=1)
    sim.tap(240, 122)  # Undo is in the deleted row
    sim.wait(page=14, pending_delete=False, shared_count=1)
    sim.tap(240, 122); sim.wait(page=30)
    sim.tap_ui("button_shared_detail_delete"); sim.wait(page=23)
    hold(sim, page=14, pending_delete=True)
    sim.tap(38, 36); sim.wait(page=11, pending_delete=False, shared_count=0)


def assert_pager(sim, name, pages, active):
    # Service mutations can precede the controller's 250ms snapshot refresh.
    # Verify the actual painted result converges, not only the backend count.
    deadline = time.monotonic() + 2
    while True:
        sim.capture(name)
        with Image.open(sim.directory / (name + '.png')) as frame:
            pixels = frame.convert('RGB')
            runs = []
            for x in range(160, 320):
                color = pixels.getpixel((x, 455))
                if max(color) > 20:
                    if not runs or runs[-1][-1][0] != x - 1:
                        runs.append([])
                    runs[-1].append((x, color))
            selected = [i for i, run in enumerate(runs)
                        if any(c[0] > 200 and c[1] < 120 for _, c in run)]
        if len(runs) == pages and selected == [active]:
            return
        if time.monotonic() >= deadline:
            assert len(runs) == pages, (name, [len(run) for run in runs])
            assert selected == [active]
        time.sleep(.1)


@pytest.mark.parametrize('sim,count,pages', [
    ('empty', 0, 2), ('pager-five', 5, 2), ('pager-six', 6, 3), ('pager-max', 16, 4),
], indirect=['sim'])
def test_pager_tracks_real_ideas_and_add_tile_remains_reachable(sim, count, pages):
    sim.wait(page=0, bundle_count=count)
    assert_pager(sim, 'home-pager', pages, 0)
    for page in range(pages - 1):
        sim.swipe(420, 250, 60, 250)
        sim.wait(page=19, grid_page=page)
        assert_pager(sim, 'ideas-pager-%d' % page, pages, page + 1)
    sim.swipe(420, 250, 60, 250)  # No phantom page after the Add Idea tile.
    sim.wait(page=19, grid_page=pages - 2)
    if not count:
        sim.capture('empty-add-idea')
        with Image.open(sim.directory / 'empty-add-idea.png') as frame:
            assert min(frame.getpixel((240, 146))[:3]) > 200  # large visible plus
            assert all(max(rgb[:3]) < 20 for rgb in frame.crop((24, 72, 150, 180)).getdata())
        sim.tap_ui('button_cell_1')
    else:
        sim.tap_ui('button_cell_%d' % (count % 6))
    sim.wait(page=4, code_length=0, install_calls=0)


@pytest.mark.parametrize('sim', ['manage-states'], indirect=True)
def test_manage_reference_status_colors_badge_and_storage_bar(sim):
    sim.manage()
    sim.wait(page=5, bundle_count=6)
    sim.capture('manage-reference')
    with Image.open(sim.directory / 'manage-reference.png') as frame:
        pixels = frame.convert('RGB')
        assert sum(max(rgb) > 160 for rgb in pixels.crop((100, 94, 456, 120)).getdata()) > 100  # live capacity text
        for y in (192, 280, 368):  # Current and the two available updates.
            assert sum(r > 200 and 30 < g < 130 and b < 90
                       for r, g, b in pixels.crop((100, y, 412, y + 24)).getdata()) > 30
        assert sum(min(pixels.getpixel((x, 131))) > 200 for x in range(24, 456)) > 200
        assert sum(r > 200 and 30 < g < 130 and b < 90
                   for r, g, b in pixels.crop((360, 16, 384, 40)).getdata()) > 200
    sim.tap_ui('manage_update_all')
    sim.wait(page=31, batch=False, check_calls=0)
    sim.tap_ui('button_update_all_accept')
    sim.wait(page=5, batch=True, check_calls=1)
    sim.tap_ui('manage_update_all')
    sim.wait(page=5, batch=False, cancel_calls=1)
    sim.rpc("wait", frames=60)
    sim.wait(page=5, batch=False)


@pytest.mark.parametrize('sim', ['scan-busy'], indirect=True)
def test_update_tap_during_scan_keeps_library_visible(sim):
    sim.manage()
    sim.wait(nand_scanning=True)
    sim.tap_ui('manage_update_all')
    sim.wait(page=5, check_calls=0)
    sim.wait(nand_scanning=False)
    sim.tap_ui('manage_update_all')
    sim.wait(page=31, check_calls=0)
    sim.tap_ui('button_update_all_accept')
    sim.wait(page=5, check_calls=1)


@pytest.mark.parametrize('sim', ['pager-six'], indirect=True)
def test_pager_shrinks_after_a_real_idea_is_deleted(sim):
    assert_pager(sim, 'before-delete', 3, 0)
    sim.manage(); sim.tap(150, 170); sim.wait(page=6)
    sim.tap_ui('button_local_delete'); sim.wait(page=23)
    hold(sim, page=5, pending_delete=True)
    sim.tap(38, 36)
    sim.wait(page=0, pending_delete=False, delete_calls=1, bundle_count=5)
    assert_pager(sim, 'after-delete', 2, 0)
    sim.swipe(420, 250, 60, 250); sim.wait(page=19, grid_page=0)
    sim.swipe(420, 250, 60, 250); sim.wait(page=19, grid_page=0)
    assert_pager(sim, 'last-page-after-delete', 2, 1)
    sim.tap_ui('button_cell_5'); sim.wait(page=4, install_calls=0)


def test_settings_scroll_follows_finger_and_hit_regions(sim):
    sim.tap(390, 360); sim.wait(page=11)
    sim.capture('settings-scroll-top')
    sim.rpc('feed_pointer', x=240, y=410, pressed=True)
    sim.rpc('feed_pointer', x=240, y=360, pressed=True)
    sim.wait(page=11, settings_offset=50)
    sim.capture('settings-scroll-mid')
    sim.rpc('feed_pointer', x=240, y=310, pressed=True)
    sim.wait(page=11, settings_offset=100)
    sim.capture('settings-scroll-held')
    with Image.open(sim.directory / 'settings-scroll-top.png') as top, \
         Image.open(sim.directory / 'settings-scroll-mid.png') as mid, \
         Image.open(sim.directory / 'settings-scroll-held.png') as held:
        assert top.crop((0, 0, 480, 92)).tobytes() == mid.crop((0, 0, 480, 92)).tobytes() == held.crop((0, 0, 480, 92)).tobytes()
        assert top.crop((16, 96, 464, 448)).tobytes() != mid.crop((16, 96, 464, 448)).tobytes()
        assert mid.crop((16, 96, 464, 448)).tobytes() != held.crop((16, 96, 464, 448)).tobytes()
    sim.rpc('feed_pointer', x=240, y=310, pressed=False)
    sim.wait(page=11, settings_offset=100)
    sim.tap(240, 208)  # Language moved here; this used to activate Display.
    sim.wait(page=13)


def test_settings_scroll_coasts_and_keeps_header_fixed(sim):
    sim.tap(390, 360); sim.wait(page=11)
    sim.rpc('feed_pointer', x=240, y=410, pressed=True)
    sim.rpc('wait', frames=2)
    sim.rpc('feed_pointer', x=240, y=370, pressed=True)
    sim.rpc('feed_pointer', x=240, y=370, pressed=False)
    sim.rpc('wait', frames=20)
    offset = json.loads(sim.state_file.read_text())['settings_offset']
    assert 40 < offset <= 280
    sim.tap(44, 48)  # Back must act even while the content is still coasting.
    sim.wait(page=0)
    sim.capture('home-no-arrow')
    with Image.open(sim.directory / 'home-no-arrow.png') as frame:
        assert min(frame.getpixel((16, 150))[:3]) > 200  # Home brand card rendered after the scroll.


@pytest.mark.parametrize('sim', ['scan-open'], indirect=True)
def test_app_confirmation_is_immediate_during_scan_and_preserves_unsaved_warning(sim):
    sim.manage(); sim.wait(nand_scanning=True)
    sim.tap(44, 48); sim.wait(page=0)
    sim.swipe(420, 250, 60, 250); sim.wait(page=19)
    sim.tap(92, 150)
    sim.wait(timeout=1.5, page=20, nand_scanning=True, install_calls=0)
    sim.capture('open-while-scanning')
    sim.tap_ui('button_open_accept')
    sim.wait(page=20, nand_scanning=True, install_calls=0)
    sim.tap_ui('button_open_cancel'); sim.wait(page=19, install_calls=0)
    sim.tap(92, 150); sim.wait(page=20, nand_scanning=True)
    sim.wait(timeout=30, page=20, nand_scanning=False, install_calls=0)
    sim.capture('open-after-scan-unsaved')
    sim.tap_ui('button_toggle_open_confirm')
    sim.wait(page=20, open_confirm=True, install_calls=0)
    sim.tap_ui('button_open_accept')
    sim.wait(timeout=45, page=8, install_calls=1, restart_calls=1)


def test_home_wifi_shortcut_and_develop_without_a_build(sim):
    sim.tap_ui('home_wifi'); sim.wait(page=1, pending=False)
    sim.tap(38, 36); sim.wait(page=0)
    sim.tap_ui('button_nav_develop'); sim.wait(page=10)
    sim.tap_ui('button_develop_save')
    sim.wait(page=10, save_calls=0, restart_calls=0)
    sim.tap_ui('button_develop_guide'); sim.wait(page=25)
    sim.tap(38, 36); sim.wait(page=10)


def test_shared_detail_help_and_key_space_use_existing_storage(sim):
    sim.tap_ui('button_nav_settings'); sim.wait(page=11)
    sim.tap_ui('button_settings_top_shared'); sim.wait(page=14, shared_count=0)
    sim.tap_ui('shared_empty_add'); sim.wait(page=15)
    sim.tap(35, 250); sim.tap(240, 430); sim.tap(35, 250)  # q_q, not a space in a key
    sim.tap(430, 430)
    sim.tap(35, 250); sim.tap(430, 430)
    sim.wait(page=14, shared_count=1)
    sim.tap_ui('button_shared_row_0'); sim.wait(page=30)
    sim.tap_ui('button_shared_detail_delete'); sim.wait(page=23)
    sim.tap(38, 36); sim.wait(page=30, pending_delete=False)
    sim.tap_ui('button_shared_detail_key'); sim.wait(page=29)
    sim.tap(38, 36); sim.wait(page=30)
    sim.tap_ui('button_shared_detail_value'); sim.wait(page=15)
    sim.tap(35, 250); sim.tap(430, 430)
    sim.wait(page=14, shared_count=1)


def test_about_licenses_scroll_and_return(sim):
    sim.tap_ui('button_nav_settings'); sim.wait(page=11)
    sim.swipe(240, 410, 240, 100); sim.wait(settings_offset=280)
    sim.tap_ui('button_settings_bottom_about'); sim.wait(page=16)
    sim.swipe(240, 420, 240, 240); sim.wait(about_offset=176)
    sim.tap_ui('button_about_licenses'); sim.wait(page=28)
    sim.swipe(240, 420, 240, 120); sim.wait(page=28)
    assert sim.wait()['licenses_offset'] > 0
    sim.tap(38, 36); sim.wait(page=16, about_offset=176)
    sim.tap(38, 36); sim.wait(page=11)


def test_keyboard_second_symbol_page_and_preview_cancel(sim):
    sim.wifi(); sim.wait(page=1)
    sim.tap(180, 250); sim.wait(page=2)
    sim.tap(40, 430)  # 123
    sim.tap(40, 370)  # #+=
    sim.tap(148, 310)  # literal | on the second symbol page
    sim.wait(password_length=1)
    sim.rpc('feed_pointer', x=35, y=250, pressed=True)
    sim.capture('symbol-preview')
    sim.rpc('feed_pointer', x=180, y=250, pressed=True)
    sim.rpc('feed_pointer', x=180, y=250, pressed=False)
    sim.wait(password_length=1)  # sliding off a key cancels that key
    sim.tap(38, 36); sim.wait(page=1, password_length=0)


@pytest.mark.parametrize("sim", ["marquee-preview", "marquee-text"], indirect=True)
def test_long_titles_scroll_in_library_details_and_preview_without_moving_controls(sim):
    def moves(name, bounds):
        sim.capture(name + "-start")
        sim.rpc("wait", frames=220)
        sim.capture(name + "-moving")
        with Image.open(sim.directory / (name + "-start.png")) as a, Image.open(sim.directory / (name + "-moving.png")) as b:
            assert a.crop(bounds).tobytes() != b.crop(bounds).tobytes()
            # Metadata, buttons and all other rows remain stationary.
            a.paste((0, 0, 0), bounds); b.paste((0, 0, 0), bounds)
            assert a.tobytes() == b.tobytes()

    sim.manage(); sim.wait(page=5)
    moves("library-marquee", (100, 156, 412, 190))
    sim.tap(150, 170); sim.wait(page=6)
    moves("details-marquee", (88, 29, 456, 69))
    sim.tap_ui("button_preview_idea"); sim.wait(page=9, idea_ready=True, download_calls=0)
    moves("preview-marquee", (88, 29, 456, 69))
    sim.tap_ui("button_cancel_idea"); sim.wait(page=6, download_calls=0, install_calls=0)
    sim.capture("details-marquee-return")
    with Image.open(sim.directory / "details-marquee-start.png") as a, Image.open(sim.directory / "details-marquee-return.png") as b:
        assert a.crop((88, 29, 456, 69)).tobytes() == b.crop((88, 29, 456, 69)).tobytes()


def assert_wide_done(sim, name):
    sim.capture(name)
    with Image.open(sim.directory / (name + ".png")) as frame:
        # One continuous primary button spans the centre and both halves.
        for x in (48, 230, 240, 250, 432):
            assert min(frame.getpixel((x, 398))[:3]) > 200
    sim.tap(430, 420)  # The far right must also activate Done, not stale Open.


@pytest.mark.parametrize("sim", ["development"], indirect=True)
def test_save_development_can_cancel_and_retry_with_a_wide_done_button(sim):
    sim.manage(); sim.tap(150, 170); sim.wait(page=6)
    sim.tap(38, 36); sim.wait(page=5)
    sim.tap(38, 36); sim.wait(page=0)  # Leave a previously selected Idea in controller state.
    sim.tap_ui("button_nav_develop"); sim.wait(page=10)
    sim.tap_ui("button_develop_save")
    sim.wait(page=7, save_calls=1, cancellable=True, development_saved=False)
    sim.tap_ui("button_cancel_download")
    sim.wait(page=8, cancel_calls=1, cancelled=True, development_saved=False, restart_calls=0)
    assert_wide_done(sim, "save-cancelled-wide-done")
    sim.wait(page=0, restart_calls=0)
    sim.tap_ui("button_nav_develop"); sim.wait(page=10)
    sim.tap_ui("button_develop_save")
    sim.wait(timeout=45, page=8, save_calls=2, development_saved=True, restart_calls=0)
    assert_wide_done(sim, "development-saved-wide-done")
    sim.wait(page=5, restart_calls=0, install_calls=0)


@pytest.mark.parametrize("sim", ["batch"], indirect=True)
@pytest.mark.parametrize("language", [1, 2])
def test_update_all_requires_confirmation_and_cancel_has_no_side_effects(sim, language):
    if language == 2:
        sim.tap_ui("button_nav_settings"); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.manage(); sim.tap_ui("manage_update_all")
    sim.wait(page=31, batch=False, check_calls=0, download_calls=0)
    sim.capture("confirm-update-all")
    sim.tap_ui("button_update_all_cancel")
    sim.wait(page=5, check_calls=0, cancel_calls=0, download_calls=0)
    sim.tap_ui("manage_update_all"); sim.wait(page=31)
    sim.control("ai"); sim.wait(page=5, check_calls=0)
    sim.tap_ui("manage_update_all"); sim.wait(page=31)
    sim.tap_ui("button_update_all_accept")
    sim.wait(page=5, batch=True, check_calls=1, install_calls=0, restart_calls=0)
    sim.tap_ui("manage_update_all")
    sim.wait(page=5, batch=False, check_calls=1, cancel_calls=1)


@pytest.mark.parametrize("sim", ["prefetched"], indirect=True)
@pytest.mark.parametrize("language", [1, 2])
def test_download_cancel_has_wide_done_and_aligned_status(sim, language):
    if language == 2:
        sim.tap_ui("button_nav_settings"); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.download(); sim.wait(page=4)
    for digit in (0, 4, 2, 7, 3, 1):
        sim.tap_ui("button_code_" + str(digit))
    sim.tap_ui("button_download_code"); sim.wait(page=9, idea_ready=True)
    sim.tap_ui("button_accept_idea"); sim.wait(page=7, cancellable=True, download_calls=1)
    sim.control("ideas-phase 0 500000 1000000 0 12 1000")
    sim.capture("download-status-aligned")
    with Image.open(sim.directory / "download-status-aligned.png") as frame:
        # Keep one status line just below the title, sharing its left edge.
        title = frame.crop((148, 50, 456, 106)).convert("L").point(lambda x: 255 if x > 100 else 0).getbbox()
        detail = frame.crop((148, 108, 456, 140)).convert("L").point(lambda x: 255 if x > 100 else 0).getbbox()
        assert title and detail and abs(title[0] - detail[0]) <= 4
        assert not any(max(rgb[:3]) > 100 for rgb in frame.crop((24, 172, 456, 196)).getdata())
        assert sum(max(rgb[:3]) > 100 for rgb in frame.crop((24, 208, 456, 298)).getdata()) > 300
    sim.tap_ui("button_cancel_download")
    sim.wait(page=8, cancelled=True, cancel_calls=1, install_calls=0, restart_calls=0)
    assert_wide_done(sim, "download-cancelled-wide-done")
    sim.wait(page=0, install_calls=0, restart_calls=0)


@pytest.mark.parametrize("language", [1, 2])
def test_nand_failure_requires_consent_and_recovers_downloads(sim, language):
    if language == 2:
        sim.tap_ui("button_nav_settings"); sim.wait(page=11)
        sim.tap_ui("button_settings_top_language"); sim.wait(page=13)
        sim.tap_ui("button_language_zh"); sim.wait(language=2)
        sim.tap(38, 36); sim.wait(page=11)
        sim.tap(38, 36); sim.wait(page=0)
    sim.control("nand-error")
    sim.wait(page=32, format_calls=0)
    sim.capture(f"nand-confirm-{language}")
    sim.tap_ui("button_format_nand_cancel")
    sim.wait(page=0, format_calls=0)
    sim.rpc("wait", frames=40)
    sim.wait(page=0, format_calls=0)  # A dismissed fault does not loop.
    sim.tap_ui("button_nav_manage")
    sim.wait(page=32, format_calls=0)
    sim.tap_ui("button_format_nand_cancel")
    sim.wait(page=5, format_calls=0)
    sim.tap_ui("button_format_nand")
    sim.wait(page=32, format_calls=0)
    sim.tap_ui("button_format_nand_accept")
    sim.wait(page=7, format_calls=1)
    sim.capture(f"nand-formatting-{language}")
    sim.tap_ui("button_format_nand_accept")
    sim.wait(page=7, format_calls=1)
    sim.control("nand-format-complete")
    sim.wait(page=8, format_calls=1, install_calls=0, restart_calls=0)
    sim.capture(f"nand-formatted-{language}")
    sim.tap_ui("button_result_done")
    sim.wait(page=5)
    sim.tap_ui("manage_download")
    sim.wait(page=1)  # Normal Wi-Fi setup is usable again.


def test_nand_format_failure_can_retry_and_busy_work_defers_prompt(sim):
    sim.control("ideas-refresh-busy")
    sim.control("nand-error")
    sim.wait(page=0, format_calls=0)
    sim.control("nand-idle")
    sim.wait(page=32)
    sim.tap_ui("button_format_nand_accept")
    sim.wait(page=7, format_calls=1)
    sim.control("nand-format-fail")
    sim.wait(page=8, format_calls=1)
    sim.tap_ui("button_result_done")
    sim.wait(page=5)
    sim.tap_ui("button_format_nand")
    sim.wait(page=32)
    sim.control("nand-recovered")
    sim.tap_ui("button_format_nand_accept")
    sim.wait(page=5, format_calls=1)  # Revalidate the fault before erasing.


def test_unrelated_failure_does_not_offer_nand_format(sim):
    sim.control("ideas-fail")
    sim.wait(page=8, format_calls=0)
