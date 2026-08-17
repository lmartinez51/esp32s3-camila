/**
 * @file ble_hue.h
 * @brief BLE Smart Light driver (Philips Hue + Tuya) — Robot HAL driver.
 *
 * Controls BLE lights over GATT with ephemeral connections
 * (connect -> write -> disconnect immediately) to minimize radio and buffer
 * usage. Maintains cached state in PSRAM for instant toggle resolution.
 *
 * Supported protocols (auto-detected during characteristic discovery):
 *  - Philips Hue: Service 932c32bd-de80-47a7-93ab-e652d82b7f1e,
 *    On/Off char be880001-eb94-4bc2-b590-e25d60909113 (0x01=ON, 0x00=OFF),
 *    Brightness char be880002-... (0x01..0xFE for 1..100%).
 *  - Tuya Smart Bulb: Service 0x1912, command char 0x2AE2
 *    (trama 0x55AA + dp de control; dp 0x01 bool on/off, dp 0x02 brillo 0..1000).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.1
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_HUE_H
#define BLE_HUE_H

#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BLE_HUE_PROFILE_ID "ble_hue"

/* Philips Hue BLE Service & Characteristic UUID strings (lowercase, no dashes) */
#define BLE_HUE_SERVICE_UUID_STR    "932c32bdde8047a793abe652d82b7f1e"
#define BLE_HUE_ON_OFF_CHAR_STR     "be880001eb944bc2b590e25d60909113"
#define BLE_HUE_BRIGHTNESS_CHAR_STR "be880002eb944bc2b590e25d60909113"

/* Capability definitions */
#define ROBOT_CAP_TURN_ON        (1u << ROBOT_ACTION_TURN_ON)
#define ROBOT_CAP_TURN_OFF       (1u << ROBOT_ACTION_TURN_OFF)
#define ROBOT_CAP_TOGGLE         (1u << ROBOT_ACTION_TOGGLE)
#define ROBOT_CAP_SET_BRIGHTNESS (1u << ROBOT_ACTION_SET_BRIGHTNESS)

#define BLE_HUE_CAP_MASK ( \
    ROBOT_CAP_TURN_ON | \
    ROBOT_CAP_TURN_OFF | \
    ROBOT_CAP_TOGGLE | \
    ROBOT_CAP_SET_BRIGHTNESS)

/**
 * @brief Initialize the Philips Hue driver descriptor and PSRAM context.
 * @return Pointer to the standard robot_driver_t descriptor.
 */
const robot_driver_t *ble_hue_driver_init(void);

/**
 * @brief Get the Philips Hue driver descriptor (static).
 */
const robot_driver_t *ble_hue_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_HUE_H */
