/**
 * @file wifi_arm.h
 * @brief Robotic-arm driver over LAN TCP (Phase 6).
 *
 * Endpoint format: "192.168.1.50:8000". Payload table (v1 ASCII, ESP32
 * servo-controller kits over TCP):
 *   GRAB "GRAB" | RELEASE "RELEASE" | ARM_UP "UP" | ARM_DOWN "DOWN"
 *   ARM_HOME "HOME" | MOVE_AXIS "AXIS:<id>:<angle>"
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef WIFI_ARM_H
#define WIFI_ARM_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_ARM_PROFILE_ID "wifi_arm"

/**
 * @brief Get the wifi_arm driver descriptor (registered by device_registry).
 */
const robot_driver_t *wifi_arm_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_ARM_H */
