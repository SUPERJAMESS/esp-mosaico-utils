#pragma once

/* Create the mandatory restart timer, register OTA state, and start Iris.
 * Call once after all pre-start providers/volumes; fatal setup errors abort. */
void recovery_ota_support_start(void);
