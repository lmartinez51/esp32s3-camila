#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_MAX_CODES_PER_ACTION 5

/**
 * @brief Dictionary of actions that can be mapped to IR remotes.
 * Designed to be passed easily by the Orchestrator or an AI function call.
 */
typedef enum {
    IR_ACTION_NONE = 0,
    IR_ACTION_MUTE,
    IR_ACTION_UNMUTE,
    IR_ACTION_TOGGLE_MUTE,
    IR_ACTION_WAKE,
    IR_ACTION_SLEEP,
    IR_ACTION_TOGGLE_SLEEP,
    IR_ACTION_OUTFIT_RED,
    IR_ACTION_OUTFIT_GREEN,
    IR_ACTION_OUTFIT_WHITE,
    IR_ACTION_OUTFIT_BARCA,
    IR_ACTION_MAX // Must remain the last element
} ir_action_t;

/**
 * @brief Initialize the IR action map cache from NVS.
 * Creates the thread-safe mutex and loads existing pairings into RAM.
 * 
 * @return ESP_OK on success
 */
esp_err_t ir_action_map_init(void);

/**
 * @brief Adds a hex code to an action, enforcing uniqueness across all actions.
 * Saves the updated mappings to NVS.
 */
esp_err_t ir_map_add_code(ir_action_t action, uint32_t hex_code);

/**
 * @brief Removes a specific hex code from any action it is mapped to.
 * Saves the updated mappings to NVS.
 */
esp_err_t ir_map_remove_code(uint32_t hex_code);

/**
 * @brief Thread-safe O(N) lookup of a hex code from the RAM cache.
 * @return The corresponding ir_action_t, or IR_ACTION_NONE if not found.
 */
ir_action_t ir_map_lookup(uint32_t hex_code);

#ifdef __cplusplus
}
#endif
