#ifndef ALERT_DISPATCHER_H
#define ALERT_DISPATCHER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t alert_dispatcher_send_alert(uint32_t timestamp_ms, float corr_drop);

#ifdef __cplusplus
}
#endif

#endif // ALERT_DISPATCHER_H
