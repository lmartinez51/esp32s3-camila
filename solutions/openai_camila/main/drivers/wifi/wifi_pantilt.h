/**
 * @file wifi_pantilt.h
 * @brief Pan-Tilt mount driver over LAN TCP (Phase 6).
 *
 * Endpoint format: "192.168.1.50:8000". Payload table (v1 ASCII, ESP32
 * servo-controller kits over TCP):
 *   PAN_LEFT "PAN_LEFT" | PAN_RIGHT "PAN_RIGHT" | TILT_UP "TILT_UP"
 *   TILT_DOWN "TILT_DOWN" | CENTER "CENTER"
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef WIFI_PANTILT_H
#define WIFI_PANTILT_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_PANTILT_PROFILE_ID "wifi_pantilt"

/**
 * @brief Get the wifi_pantilt driver descriptor (registered by device_registry).
 */
const robot_driver_t *wifi_pantilt_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PANTILT_H */
