/**
 * @file ble_elegoo_bt16.h
 * @brief ELEGOO BT16 robot car — Robot HAL driver (Phase 2).
 *
 * Thin driver wrapper around the legacy BLE device control module
 * (ble_device_control.h). The legacy module keeps ownership of NimBLE,
 * discovery, connection state and the ELEGOO command table; this driver
 * exposes that path through the protocol-agnostic robot_driver_t vtable.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_ELEGOO_BT16_H
#define BLE_ELEGOO_BT16_H

#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Get the ELEGOO BT16 driver descriptor (static).
 *
 * The returned descriptor must NOT be modified or freed. Register it
 * with robot_hal_register_driver() during robot_registry_init().
 */
const robot_driver_t *ble_elegoo_bt16_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_ELEGOO_BT16_H */
