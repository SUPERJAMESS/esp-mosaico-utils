// SPDX-License-Identifier: Apache-2.0
#include "vibe_battery.h"
#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include <string.h>

/* Borrow the BSP bus/profile; own only this read-only device handle. BSP's
 * legacy scalar getters discard I2C errors, so telemetry reads checked words
 * through the public IDF API. No control/configuration register is written. */
static i2c_master_dev_handle_t s_device;
static uint16_t word(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }

esp_err_t vibe_battery_init(void)
{
    if (s_device) return ESP_OK;
    esp_err_t err = bsp_battery_init();
    if (err != ESP_OK) return err;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_BATTERY_I2C_ADDR,
        .scl_speed_hz = BSP_BATTERY_I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t device = NULL;
    err = i2c_master_bus_add_device(bus, &config, &device);
    if (err == ESP_OK) s_device = device;
    return err;
}

esp_err_t vibe_battery_read(vibe_battery_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_device) return ESP_ERR_INVALID_STATE;
    /* Use the documented two-byte standard commands independently; a failed
     * word invalidates the whole observation, never a synthetic zero sample. */
    static const uint8_t commands[] = {0x08, 0x0a, 0x0c, 0x2c};
    uint16_t values[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t bytes[2];
        esp_err_t err = i2c_master_transmit_receive(s_device, &commands[i], 1, bytes, 2, 100);
        if (err != ESP_OK) return err;
        values[i] = word(bytes);
    }
    const vibe_battery_sample_t sample = {
        .voltage_mv = (int16_t)values[0], .status_flags = values[1],
        .current_ma = (int16_t)values[2], .state_of_charge = values[3],
    };
    /* Preserve complete raw readings for diagnostics even when validation
     * fails. Callers must check the result before publishing battery state. */
    *out = sample;
    if (!(sample.status_flags & (1U << 3))) return ESP_ERR_NOT_FOUND; /* BATTPRES */
    if (sample.voltage_mv <= 0 || sample.state_of_charge > 100)
        return ESP_ERR_INVALID_RESPONSE;
    /* Status/current are separate gauge reads and may straddle a charge
     * transition. Keep measured SOC/voltage valid; the caller requires both
     * positive current and a cleared DSG flag before reporting charging. */
    return ESP_OK;
}

esp_err_t vibe_battery_deinit(void)
{
    if (!s_device) return ESP_OK;
    esp_err_t err = i2c_master_bus_rm_device(s_device);
    if (err == ESP_OK) s_device = NULL;
    return err;
}
