"""Exercise Vibe's SPI initialization boundary without a display or BSP checkout."""
from pathlib import Path
import shutil
import subprocess


def test_lcd_dma_budget_and_bus_isolation(tmp_path):
    source = Path(__file__).resolve().parents[1] / (
        "firmware/vibe-mode/components/app_ui/src/vibe_lcd_budget.c")
    (tmp_path / "sdk.h").write_text(r'''
#pragma once
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
typedef int spi_host_device_t;
typedef int spi_dma_chan_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct { int max_transfer_sz, flags, mosi_io_num; } spi_bus_config_t;
typedef struct { int cs_gpio_num, trans_queue_depth, pclk_hz, flags; void *user_ctx; }
    esp_lcd_panel_io_spi_config_t;
#define BSP_LCD_H_RES 480
#define BSP_LCD_BITS_PER_PIXEL 16
#define BSP_LCD_SPI_HOST 1
#define BSP_LCD_CS 50
#define ESP_LOGI(...) ((void)0)
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t esp_lcd_new_panel_io_spi(int, const esp_lcd_panel_io_spi_config_t *, void **);
''')
    for name in ("bsp/esp_mosaico.h", "driver/spi_master.h", "esp_lcd_panel_io.h", "esp_log.h"):
        header = tmp_path / name
        header.parent.mkdir(exist_ok=True)
        header.write_text('#include "sdk.h"\n')
    (tmp_path / "driver.c").write_text(r'''
#include "sdk.h"
int result, last_host, last_dma;
spi_bus_config_t last_bus;
esp_lcd_panel_io_spi_config_t last_io;
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma) {
    last_host = host; last_dma = dma;
    if (!config) return -1;
    last_bus = *config;
    return result;
}
esp_err_t esp_lcd_new_panel_io_spi(int bus, const esp_lcd_panel_io_spi_config_t *config, void **out) {
    last_host = bus;
    if (!config) return -1;
    last_io = *config;
    if (!result && out) *out = (void *)123;
    return result;
}
''')
    (tmp_path / "main.c").write_text(r'''
#include "sdk.h"
extern int result, last_host, last_dma;
extern spi_bus_config_t last_bus;
extern esp_lcd_panel_io_spi_config_t last_io;
int main(void) {
    spi_bus_config_t bus = {.max_transfer_sz = 7680, .flags = 17, .mosi_io_num = 22};
    esp_lcd_panel_io_spi_config_t io = {50, 10, 40000000, 29, (void *)456};
    void *handle = NULL;
    assert(spi_bus_initialize(1, &bus, 3) == 0);
    assert(last_host == 1 && last_dma == 3 && last_bus.flags == 17 && last_bus.mosi_io_num == 22);
    assert(last_bus.max_transfer_sz == 3840 && bus.max_transfer_sz == 7680);
    assert(esp_lcd_new_panel_io_spi(1, &io, &handle) == 0 && handle == (void *)123);
    assert(last_io.trans_queue_depth == 1 && last_io.pclk_hz == 40000000);
    assert(last_io.flags == 29 && last_io.user_ctx == (void *)456 && io.trans_queue_depth == 10);
    assert(last_bus.max_transfer_sz * last_io.trans_queue_depth <= 4096);
    /* NAND/other devices retain every caller setting. */
    assert(spi_bus_initialize(2, &bus, 4) == 0 && last_dma == 4);
    assert(!memcmp(&last_bus, &bus, sizeof(bus)));
    assert(esp_lcd_new_panel_io_spi(2, &io, &handle) == 0);
    assert(!memcmp(&last_io, &io, sizeof(io)));
    io.cs_gpio_num = 51;
    assert(esp_lcd_new_panel_io_spi(1, &io, &handle) == 0 && last_io.trans_queue_depth == 10);
    bus.max_transfer_sz = 512;
    assert(spi_bus_initialize(1, &bus, 3) == 0 && last_bus.max_transfer_sz == 512);
    bus.max_transfer_sz = 0;
    assert(spi_bus_initialize(1, &bus, 3) == 0 && last_bus.max_transfer_sz == 3840);
    /* Propagate SDK errors and leave the caller's output untouched on failure. */
    result = -7; handle = (void *)789; io.cs_gpio_num = 50;
    assert(spi_bus_initialize(1, &bus, 3) == -7);
    assert(esp_lcd_new_panel_io_spi(1, &io, &handle) == -7 && handle == (void *)789);
    assert(spi_bus_initialize(1, NULL, 3) == -1);
    assert(esp_lcd_new_panel_io_spi(1, NULL, &handle) == -1);
    return 0;
}
''')
    executable = tmp_path / "lcd-budget"
    compiler = [shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(tmp_path)]
    symbols = ("spi_bus_initialize", "esp_lcd_new_panel_io_spi")
    objects = []
    # Bind the same wrapper/real boundary explicitly: Darwin's linker has no
    # GNU --wrap option. Production firmware still uses its IDF linker flags.
    for name, unit, prefix in (("budget", source, ""), ("driver", tmp_path / "driver.c", "__real_"),
                               ("main", tmp_path / "main.c", "__wrap_")):
        obj = tmp_path / (name + ".o")
        defines = [f"-D{symbol}={prefix}{symbol}" for symbol in symbols] if prefix else []
        subprocess.run([*compiler, *defines, "-c", str(unit), "-o", str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run([*compiler, *objects, "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
