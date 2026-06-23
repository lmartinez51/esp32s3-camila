#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_claw_init(void);
esp_err_t esp_claw_execute_script(const char* script);

#ifdef __cplusplus
}
#endif
