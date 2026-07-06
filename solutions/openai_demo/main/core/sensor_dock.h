/**
 * @file sensor_dock.h
 * @brief Sensor Dock detection and AHT30 temperature sensor management.
 *
 * Manages the external I2C sensor dock initialization, hardware radar
 * presence detection via I2C ping, and AHT30 temperature/humidity reads.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Global flag indicating whether the hardware radar dock is present.
 *
 * Set to true during sensor_dock_init() if a device is detected at address
 * 0x28 on the external I2C bus. Read by the orchestrator and CSI subsystem
 * to decide which motion detection strategy to use.
 */
extern bool g_hardware_radar_present;

/**
 * @brief Indicates whether the AHT30 sensor was found and initialized.
 *
 * Exposed so the orchestrator can adjust its polling interval and enable
 * temperature display on the UI.
 */
extern bool s_aht30_present;

/**
 * @brief Timestamp (ms) of the last successful AHT30 temperature poll.
 *
 * Updated by sensor_dock_poll_temperature() and read by the orchestrator
 * task to enforce the 10-minute polling interval.
 */
extern uint32_t s_last_aht30_poll_ms;

/**
 * @brief Returns true if the external sensor dock is connected.
 *
 * Thin wrapper around @p g_hardware_radar_present for callers that
 * prefer a function interface.
 */
bool hardware_is_sensor_dock_connected(void);

/**
 * @brief Initialize the external I2C bus, detect the sensor dock, and
 *        initialize the AHT30 sensor if present.
 *
 * Must be called once from app_main after all core primitives are ready.
 * Sets g_hardware_radar_present and s_aht30_present accordingly.
 */
void sensor_dock_init(void);

/**
 * @brief Read the AHT30 temperature and update the Dr. Simi UI overlay.
 *
 * No-op if the AHT30 is not present. Logs a warning on read failure but
 * continues gracefully (hardware optionality).
 */
void sensor_dock_poll_temperature(void);
