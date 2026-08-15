/**
 * @file ble_generic_nus.h
 * @brief Generic NUS / serial BLE driver (Phase 3).
 *
 * Controls devices exposing the Nordic UART Service (NUS) or other
 * proprietary serial profiles. The driver maps normalized HAL actions to
 * ASCII payloads (default table below), sends them over the device's GATT
 * value handle and supports pulse-stop for timed motion commands.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_GENERIC_NUS_H
#define BLE_GENERIC_NUS_H

#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BLE_GENERIC_NUS_PROFILE_ID "ble_generic_nus"

/* Nordic UART Service (lowercase, no dashes — matches registry format) */
#define BLE_GENERIC_NUS_SERVICE_UUID_STR "6e400001b5a3f393e0a9e50e24dcca9e"

/**
 * @brief Get the generic NUS driver descriptor (static).
 */
const robot_driver_t *ble_generic_nus_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GENERIC_NUS_H */
