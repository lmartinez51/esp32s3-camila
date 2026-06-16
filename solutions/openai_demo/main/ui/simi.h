/**
 * @file simi.h
 * @brief Dr. Simi procedural mascot — public API and state model.
 *
 *        Phase 1: off-screen canvas + parametric face renderer + static draw per state.
 *        Phases 2-3 (animation task, transitions) build on the same face model.
 *
 * @author Lorenzo Martínez
 * @platform ESP32-S3-BOX3
 */
#ifndef MAIN_UI_SIMI_H
#define MAIN_UI_SIMI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief High-level visual states mapped from the device FSM / UI events.
 */
typedef enum
{
    SIMI_STATE_BOOT = 0,   /**< First appearance, friendly intro. */
    SIMI_STATE_IDLE,       /**< Awake but quiet (e.g. WiFi up, pre-session). */
    SIMI_STATE_LISTENING,  /**< WebRTC active, attentive. */
    SIMI_STATE_THINKING,   /**< Tool call / processing. */
    SIMI_STATE_TALKING,    /**< Assistant speaking. */
    SIMI_STATE_HAPPY,      /**< Success / greeting. */
    SIMI_STATE_MUTED,      /**< Microphone muted. */
    SIMI_STATE_ALERT,      /**< Intruder / vigilante alarm. */
    SIMI_STATE_SAD,        /**< Disconnected / error. */
    SIMI_STATE_SLEEP,      /**< Going to sleep (eyes closed). */
    SIMI_STATE_MAX
} simi_state_t;

/**
 * @brief Parametric description of Dr. Simi's face for one frame.
 *        Expressions and animation are just changes to these fields.
 */
typedef struct
{
    int16_t eye_open;    /**< 0..100 % eyelid open (blink). */
    int16_t mouth_curve; /**< -100 frown .. +100 smile. */
    int16_t mouth_open;  /**< 0..100 jaw open (talking). */
    int16_t brow_angle;  /**< -100 sad .. +100 angry. */
    int16_t head_dy;     /**< Vertical head offset in px (bob). */
    bool eyes_up;        /**< Look upward (thinking). */
    bool blush;          /**< Cheek blush overlay. */
    bool alert_border;   /**< Draw red alarm border on screen. */
    uint16_t bg;         /**< Canvas background color. */
} simi_face_t;

/**
 * @brief Allocates the off-screen canvas (prefers PSRAM, falls back to internal RAM).
 *        Must be called after ui_init() (panel + mutex ready).
 * @return ESP_OK, or ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t ui_simi_init(void);

/**
 * @brief Whether the canvas is allocated and ready to draw.
 */
bool ui_simi_ready(void);

/**
 * @brief Fills the default face parameters for a given state.
 * @param state  Target visual state.
 * @param out    Face struct to populate (must be non-NULL).
 */
void simi_face_for_state(simi_state_t state, simi_face_t *out);

/**
 * @brief Renders a single static frame for @p state and blits it centered.
 *        Phase 1 entry point (no animation).
 */
void ui_simi_render_static(simi_state_t state);

/**
 * @brief Starts the low-duty Dr. Simi animation runtime.
 *        The canvas must already be initialized with ui_simi_init().
 */
esp_err_t ui_simi_start(void);

/**
 * @brief Stops the animation task and waits briefly until it exits.
 */
void ui_simi_stop(void);

/**
 * @brief Updates the target visual state. Last state wins; no event backlog is kept.
 */
void ui_simi_set_state(simi_state_t state);

/**
 * @brief Tells the runtime whether assistant output audio is actively playing.
 */
void ui_simi_notify_speaking(bool active);

/**
 * @brief Sets an overlay text (like a thought bubble) to be rendered on top of Dr. Simi.
 *        Pass NULL to clear the text.
 */
void ui_simi_set_overlay_text(const char *text, uint16_t color);

/**
 * @brief Sets the temperature overlay text (AHT30 sensor) to be rendered on top of Dr. Simi.
 *        Pass NULL to clear the text.
 */
void ui_simi_set_temperature_text(const char *text);

/**
 * @brief Releases the canvas buffer.
 */
void ui_simi_deinit(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_SIMI_H */
