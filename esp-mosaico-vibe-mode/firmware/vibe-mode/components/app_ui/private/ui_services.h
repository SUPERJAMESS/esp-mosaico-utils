#pragma once
#include "vibe_ui.h"
/* Process-lifetime service projection; consumed only by the render task. */
const vibe_services_t *factory_ui_services(void);
