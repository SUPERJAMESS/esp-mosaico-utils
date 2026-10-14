// SPDX-License-Identifier: Apache-2.0
#include "bsp/esp_mosaico.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

/* The BSP owns panel wiring, timing and PSRAM DMA policy. Vibe Mode also runs
 * Wi-Fi and a persistent HP SRAM Ideas stack, so bound the LCD driver's
 * temporary DMA copies here. One DMA descriptor and one in-flight transaction
 * bound temporary payload storage to about 4 KiB. Request four RGB565 lines to
 * fit one descriptor even after IDF alignment; its actual limit rounds up
 * according to DMA alignment. The BSP default needs two. Reclaim the
 * previous transfer before allocating the next; never enable PSRAM DMA as a
 * memory-pressure fallback. Keep this product policy out of other SPI buses. */
enum { LCD_DMA_BYTES = BSP_LCD_H_RES * 4 * BSP_LCD_BITS_PER_PIXEL / 8 };

esp_err_t __real_spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config,
                                    spi_dma_chan_t dma);
esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                         const esp_lcd_panel_io_spi_config_t *config,
                                         esp_lcd_panel_io_handle_t *out);

esp_err_t __wrap_spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config,
                                    spi_dma_chan_t dma) {
    if (host != BSP_LCD_SPI_HOST || !config)
        return __real_spi_bus_initialize(host, config, dma);
    spi_bus_config_t bounded = *config;
    if (bounded.max_transfer_sz <= 0 || bounded.max_transfer_sz > LCD_DMA_BYTES)
        bounded.max_transfer_sz = LCD_DMA_BYTES;
    return __real_spi_bus_initialize(host, &bounded, dma);
}

esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                         const esp_lcd_panel_io_spi_config_t *config,
                                         esp_lcd_panel_io_handle_t *out) {
    if (bus != BSP_LCD_SPI_HOST || !config || config->cs_gpio_num != BSP_LCD_CS)
        return __real_esp_lcd_new_panel_io_spi(bus, config, out);
    esp_lcd_panel_io_spi_config_t bounded = *config;
    bounded.trans_queue_depth = 1;
    ESP_LOGI("vibe_lcd", "DMA bus request: %u bytes, one queued transfer", LCD_DMA_BYTES);
    return __real_esp_lcd_new_panel_io_spi(bus, &bounded, out);
}
