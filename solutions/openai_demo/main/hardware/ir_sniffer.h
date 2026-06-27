#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "ir_action_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the background IR sniffer task using the RMT RX peripheral.
 * 
 * @return ESP_OK on success
 */
esp_err_t ir_sniffer_init(void);


/**
 * @brief Sends a standard NEC protocol frame via the RMT TX peripheral
 * 
 * @param hex_code The 32-bit hex code to transmit
 * @return ESP_OK on success, or an esp_err_t from the RMT driver
 */
esp_err_t ir_transmitter_send_raw(uint32_t hex_code);

/**
 * @brief Places the sniffer into intercept mode to pair the next received IR code.
 * Automatically times out after 30 seconds.
 */
void ir_sniffer_enter_pairing_mode(ir_action_t target_action);

#ifdef __cplusplus
}
#endif
