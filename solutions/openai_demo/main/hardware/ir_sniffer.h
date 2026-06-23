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

// --- NEC IR Dictionary Macros ---
#define TV_CMD_POWER 0xFD020707
#define TV_CMD_VOL_UP 0xF8070707
#define TV_CMD_VOL_DOWN 0xF40B0707
#define TV_CMD_CH_UP 0xED120707
#define TV_CMD_CH_DOWN 0xEF100707
#define TV_CMD_MUTE 0xF00F0707
#define TV_CMD_NUM_1 0xFB040707
#define TV_CMD_NUM_2 0xFA050707
#define TV_CMD_NUM_3 0xF9060707
#define TV_CMD_NUM_4 0xF7080707
#define TV_CMD_NUM_5 0xF6090707
#define TV_CMD_NUM_6 0xF50A0707
#define TV_CMD_NUM_7 0xF30C0707
#define TV_CMD_NUM_8 0xF20D0707
#define TV_CMD_NUM_9 0xF10E0707
#define TV_CMD_NUM_0 0xEE110707
#define TV_CMD_DASH 0xDC230707

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
