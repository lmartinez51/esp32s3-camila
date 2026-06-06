#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t csi_handler_start(void);
void csi_handler_stop(void);

#ifdef __cplusplus
}
#endif
