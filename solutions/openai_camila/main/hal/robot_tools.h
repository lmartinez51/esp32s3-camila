/**
 * @file robot_tools.h
 * @brief Robot tool catalog builder — generates the WebRTC session.update
 * tool definitions from the HAL catalog.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef ROBOT_TOOLS_H
#define ROBOT_TOOLS_H

#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Appends the robot tool definitions to a cJSON tools array.
 *
 * Phase 1: the legacy BLE tools byte-identical to the previous hardcoded
 * JSON (get_discovered_ble_devices, control_ble_device, set_ble_device_alias).
 * Phase 6: `control_robot` is generated dynamically from the registered
 * HAL driver catalog (multi-protocol: BLE/WiFi/IR), with the static
 * `control_ble_device` kept as an adapter-level alias.
 *
 * @param tools Pointer to an existing cJSON array ("tools").
 * @return Number of tools appended, or -1 on invalid input.
 */
int robot_tools_append_tools_json(cJSON *tools);

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_TOOLS_H */
