#include "esp_iris_internal.h"

#include <stdio.h>

extern void __real_app_main(void);

/* The scheduler, VFS and configured PSRAM are ready here. Capture only: NVS,
 * identity and the single USB driver still belong to esp_iris_start(). */
void __wrap_app_main(void)
{
    const esp_err_t err = iris_log_capture_start(&g_iris);
    if (err != ESP_OK) {
        fprintf(stderr, "ESP-Iris early log capture failed (%d)\n", (int)err);
    }
    __real_app_main();
}
