#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "ir_action_map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_CMD_IR_LEARNED "LUA_CMD_IR_LEARNED"
#define LUA_CMD_IR_TIMEOUT "LUA_CMD_IR_TIMEOUT"

typedef enum {
    IR_MODE_RECEIVER = 0,
    IR_MODE_LEARNING
} ir_sniffer_mode_t;

/**
 * @brief Starts the learning mode with a 15-second hardware timeout.
 */
void ir_sniffer_start_learning(void);

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


#ifdef __cplusplus
}
#endif
