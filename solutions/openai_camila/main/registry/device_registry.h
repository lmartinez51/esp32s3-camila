/**
 * @file device_registry.h
 * @brief Robot registry bootstrap — drivers + NVS migration (Phase 2).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Initialize the robot registry.
 *
 * Idempotent. Calls robot_hal_init(), registers the ELEGOO BT16 driver
 * and spawns the NVS migration task (internal SRAM stack, low priority,
 * core 0) that watches WIFI_CONNECTED_BIT and migrates legacy profiles
 * on every (re)connect.
 *
 * @return ESP_OK on success.
 */
esp_err_t robot_registry_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_REGISTRY_H */
