"""Configuration and linker-ledger regressions for the internal-RAM profile."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/firmware/iris_internal_budget"
spec = importlib.util.spec_from_file_location("iris_budget_map", FIXTURE / "map_report.py")
ledger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ledger)


def test_normal_profiles_bound_internal_buffers():
    for name in ("hello_world",):
        values = dict(line.split("=", 1) for line in
                      (ROOT.parent / "mosaico-tools/templates" / name / "sdkconfig.defaults").read_text().splitlines()
                      if line.startswith("CONFIG_") and "=" in line)
        for key in ("WIRE_BUFFERS_PSRAM", "SERVICE_CONTEXT_PSRAM", "SERVICE_STATE_PSRAM",
                    "RPC_RESPONSE_PSRAM", "SERVICE_PERSISTENT", "LOG_RING_STORAGE_PSRAM",
                    "TCP_DEFER_UNTIL_NETIF"):
            assert values["CONFIG_ESP_IRIS_" + key] == "y"
        assert values["CONFIG_ESP_IRIS_SERVICE_STACK_SIZE"] == "4096"
        assert values["CONFIG_ESP_IRIS_USB_TASK_STACK_SIZE"] == "2048"
        for key in ("RX_BUFSIZE", "TX_BUFSIZE", "EP_BUFSIZE"):
            assert values["CONFIG_TINYUSB_CDC_" + key] == ("512" if key == "EP_BUFSIZE" else "1024")


def test_fixture_preserves_partitions_and_uses_real_ui():
    assert (FIXTURE / "partitions.csv").read_text() == (ROOT.parent / "mosaico-tools/templates/hello_world/partitions.csv").read_text()
    cmake = (FIXTURE / "CMakeLists.txt").read_text()
    assert 'MOSAICO_TEST_APPLICATION' in cmake
    assert 'target_sources(${app_main}' in cmake


@pytest.mark.parametrize("component_layout", [True, False])
def test_fixture_discovers_application_components(tmp_path, component_layout):
    cmake = shutil.which("cmake")
    if cmake is None:
        pytest.skip("CMake is required to check fixture component discovery")
    application = tmp_path / "application with spaces"
    (application / "main").mkdir(parents=True)
    (application / "main/CMakeLists.txt").write_text("# application entrypoint\n")
    if component_layout:
        for name in ("app_display", "app_hello"):
            component = application / "components" / name
            component.mkdir(parents=True)
            (component / "CMakeLists.txt").write_text("# application component\n")
    fake_idf = tmp_path / "idf"
    entry = fake_idf / "tools/cmake/project.cmake"
    entry.parent.mkdir(parents=True)
    entry.write_text(
        'file(WRITE "${CMAKE_BINARY_DIR}/component-dirs.txt" "${EXTRA_COMPONENT_DIRS}")\n'
        'message(FATAL_ERROR "test reached IDF component discovery")\n'
    )
    build = tmp_path / "build"
    result = subprocess.run(
        [cmake, "-S", str(FIXTURE), "-B", str(build),
         "-DMOSAICO_TEST_APPLICATION=" + str(application),
         "-DGSPC_EXECUTABLE=" + str(tmp_path / "unused-gspc")],
        env=dict(os.environ, IDF_PATH=str(fake_idf)),
        capture_output=True, text=True, timeout=30,
    )
    assert "test reached IDF component discovery" in result.stdout + result.stderr
    search_paths = [Path(value).resolve() for value in
                    (build / "component-dirs.txt").read_text().split(";")]
    # Match IDF's discovery of direct components and directories containing them.
    found = set()
    for directory in search_paths:
        assert directory.is_dir()
        if (directory / "CMakeLists.txt").is_file():
            found.add(directory.name)
        else:
            found.update(path.parent.name for path in directory.glob("*/CMakeLists.txt"))
    assert "main" in found
    if component_layout:
        assert {"app_display", "app_hello"} <= found


def test_map_ledger_counts_only_live_internal_input_sections():
    report = ledger.summarize("""
 .bss.discarded 0x00000000 0x8000 esp-idf/esp_iris/libesp_iris.a(foo.c.obj)
.dram0.data 0x2f010000 0x100
 .data.g_iris 0x2f010000 0x40 esp-idf/esp_iris/libesp_iris.a(esp_iris.c.obj)
.dram0.bss 0x2f010100 0x1000
 .bss.usb
                0x2f010100 0x800 esp-idf/espressif__tinyusb/libespressif__tinyusb.a(cdc.c.obj)
 .bss.s_records 0x2f010900 0x100 esp-idf/main/libmain.a(budget.c.obj)
 .bss.s_mirror 0x2f010a00 0x40 esp-idf/main/libmain.a(iris_screen_mirror.c.obj)
.iram0.text 0x2f000000 0x1000
 .text.usb_irq 0x2f000000 0x30 esp-idf/esp_hal_usb/libesp_hal_usb.a(usb.c.obj)
.flash.rodata 0x50010000 0x1000
 .rodata.usb 0x50010000 0x200 esp-idf/espressif__tinyusb/libespressif__tinyusb.a(cdc.c.obj)
""")
    assert report["static_internal_data_bytes"] == 0x880
    assert report["resident_iram_bytes"] == 0x30


def test_map_ledger_conservatively_charges_four_byte_alignment():
    report = ledger.summarize("""
.dram0.bss 0x2f010000 0x100
 .bss.flag 0x2f010000 0x1 esp-idf/esp_iris/libesp_iris.a(esp_iris.c.obj)
 .bss.usb 0x2f010004 0x7 esp-idf/espressif__tinyusb/libespressif__tinyusb.a(cdc.c.obj)
""")
    assert report["static_internal_data_bytes"] == 12
    assert report["static_alignment_charge_bytes"] == 4
