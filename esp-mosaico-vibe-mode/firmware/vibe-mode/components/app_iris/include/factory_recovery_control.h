#pragma once

#include "esp_err.h"
#include "app_iris_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register USB-only Recovery control methods used to configure Wi-Fi and to
 * open the download page and query the Bridge session. Copies ui callbacks;
 * ui->ctx must remain valid until reboot. Returns registration/config errors. */
esp_err_t factory_recovery_control_register(const app_iris_ui_t *ui);

#ifdef __cplusplus
}
#endif
