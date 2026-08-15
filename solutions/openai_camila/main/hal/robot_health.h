/**
 * @file robot_health.h
 * @brief Robot HAL health monitor (Phase 7): heap/PSRAM budget + registry
 * integrity, on a periodic low-priority task (PSRAM stack).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef ROBOT_HEALTH_H
#define ROBOT_HEALTH_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Start the health monitor task (idempotent). Tick interval:
 * CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S.
 */
esp_err_t robot_health_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_HEALTH_H */
