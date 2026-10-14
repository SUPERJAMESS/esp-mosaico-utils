#pragma once
#include "esp_err.h"
/* Before esp_iris_start: publish the optional NAND volume and register its
 * update RPC. Mount failure is logged but does not fail USB maintenance. */
esp_err_t app_iris_storage_register(void);
