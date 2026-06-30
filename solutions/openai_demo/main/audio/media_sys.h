#pragma once
#include <stdbool.h>
#include "esp_webrtc.h"
/**
 * @brief Thread-safe setter for the Vigilante microphone mute state.
 * @param muted If true, microphone data is replaced with deterministic dither.
 */
void media_sys_set_vigilante_mute(bool muted);

/**
 * @brief Initializes the media system.
 * @return 0 on success, non-zero on failure.
 */
int media_sys_buildup(void);

/**
 * @brief Checks if the media system is initialized and ready.
 * @return true if ready, false otherwise.
 */
bool media_sys_is_ready(void);

/**
 * @brief Tears down the media system and frees resources.
 */
void media_sys_teardown(void);

/**
 * @brief Restarts the audio capture path.
 * @return true on success, false on failure.
 */
bool media_sys_restart_capture(void);

/**
 * @brief Gets the WebRTC media provider interface.
 * @param provide Pointer to the media provider structure to populate.
 * @return 0 on success, non-zero on failure.
 */
int media_sys_get_provider(esp_webrtc_media_provider_t *provide);
