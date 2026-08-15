/**
 * @file wifi_http.h
 * @brief HTTP REST driver for LAN robots (Phase 4).
 *
 * Endpoint format: "192.168.1.50:8000". Commands are POSTed as JSON to
 * http://<ip>:<port><CONFIG_ROBOT_HTTP_COMMAND_PATH> ("/command" default):
 *   {"action": "FORWARD", "speed": 70, "duration_ms": 1000}
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef WIFI_HTTP_H
#define WIFI_HTTP_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_HTTP_PROFILE_ID "wifi_http"

/**
 * @brief Get the wifi_http driver descriptor (registered by device_registry).
 */
const robot_driver_t *wifi_http_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HTTP_H */
