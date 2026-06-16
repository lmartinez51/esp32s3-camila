#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the bare-metal Hardware Abstraction Layer for the Human Presence Radar.
 * Configures GPIO_NUM_21 as an input with an interrupt on the positive edge, but keeps it disabled.
 * 
 * @return ESP_OK on success.
 */
esp_err_t radar_hal_init(void);

/**
 * @brief Enables the radar hardware interrupt (One-shot mode).
 * Clears any pending interrupts to prevent immediate false positives.
 */
void radar_hal_enable(void);

/**
 * @brief Disables the radar hardware interrupt.
 */
void radar_hal_disable(void);

#ifdef __cplusplus
}
#endif
