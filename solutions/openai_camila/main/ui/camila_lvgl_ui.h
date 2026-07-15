#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATE_BOOT = 0,          // System booting. Color: Neon Violet (#8A2BE2).
    UI_STATE_WIFI_CONNECTING,   // Network connection. Color: Sunset Orange (#FF8C00).
    UI_STATE_BLE_SCAN,          // Identity validation. Color: Electric Cyan (#00FFFF) [Pulsing].
    UI_STATE_SUCCESS,           // Handshake complete. Color: Emerald Green (#50C878).
    UI_STATE_ACTIVE_WEBRTC,     // Active session. Color: Soft White (#F5F5F5) [Breathing].
    UI_STATE_ALERT_VIGILANTE,   // Security alert / Killswitch. Color: Crimson Red (#DC143C).
    UI_STATE_ERROR              // System failure. Color: Flashing Red.
} ui_state_t;

/**
 * @brief Initialize the LVGL UI Engine (Spawns background task)
 */
void camila_lvgl_init(void);

/**
 * @brief Thread-safe function to update the UI state and text.
 * 
 * @param state The new UI macro state (determines ring color and animations).
 * @param title The large macro text to display (e.g. "LISTENING"). Can be NULL to keep previous.
 * @param subtitle The smaller micro-action text (e.g. "Analyzing audio..."). Can be NULL to keep previous.
 */
void camila_ui_update_state(ui_state_t state, const char* title, const char* subtitle);

/**
 * @brief Transitions the UI from the text-based boot screen to the Avatar mode.
 * Hides the boot container and displays the background + mouth sprite.
 */
void camila_ui_show_avatar(void);

/**
 * @brief Swaps the mouth sprite to animate speaking.
 * 
 * @param is_speaking True for open mouth, False for closed mouth.
 */
void camila_ui_set_speaking_state(bool is_speaking);

#ifdef __cplusplus
}
#endif
