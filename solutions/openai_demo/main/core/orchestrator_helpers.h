/**
 * @file orchestrator_helpers.h
 * @brief Shared state, types, constants, and helper functions for the
 *        orchestrator state machine.
 *
 * This header is the single point of truth for:
 *   - orchestrator_state_t enum
 *   - Timing / depth #defines
 *   - All shared orchestrator variables (extern declarations)
 *   - All orchestrator helper function declarations
 *
 * Every orchestrator sub-module (tasks, vigilante, FSM) includes this header.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "app_events.h"
#include "common.h"   /* webrtc_session_mode_t, start_webrtc, stop_webrtc */

/* ── Orchestrator State Enum ────────────────────────────────────────────── */

typedef enum
{
    STATE_WAIT_WIFI = 0,
    STATE_SLEEP,
    STATE_PREPARING_BLE,
    STATE_VALIDATING_IDENTITY,
    STATE_RELEASING_BLE,
    STATE_DISPATCHING_ALERT,
    STATE_IGNITING,
    STATE_ACTIVE,
    STATE_AUTO_SLEEPING,
    STATE_STOPPING_WEBRTC,
    STATE_FATAL_ERROR,
} orchestrator_state_t;

/* ── Timing and Queue Constants ─────────────────────────────────────────── */

#define ORCHESTRATOR_QUEUE_DEPTH              8
#define ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS    200
#define IDENTITY_VALIDATION_TIMEOUT_MS        BLE_DEVICE_SCAN_TIMEOUT_MS
#define BLE_RELEASE_TIMEOUT_MS                5000
#define AUTO_SLEEP_TIMEOUT_MS                 (3 * 60 * 1000)
#define AUTO_SLEEP_POLL_MS                    1000
#define VIGILANTE_REINFORCEMENT_DELAY_MS      30000
#define VIGILANTE_VACATED_CONFIRM_MS          10000
#define VIGILANTE_ACTIVE_TIMEOUT_MS           (5 * 60 * 1000)
#define VIGILANTE_STATUS_STALE_MS             5000
#define SLEEP_CSI_COOLDOWN_MS                 4000
#define SLEEP_WIFI_READY_DISPLAY_MS           900
#define WEBRTC_STOP_TIMEOUT_MS                8000
#define WEBRTC_STOP_TASK_STACK_SIZE           (6 * 1024)
#define WEBRTC_STOP_TASK_PRIORITY             (tskIDLE_PRIORITY + 2)
#define ORCHESTRATOR_EXTERNAL_STACK_CAPS      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ── Shared Global Primitives ───────────────────────────────────────────── */

/** Application startup event group (WiFi / WebRTC bits). */
extern EventGroupHandle_t app_startup_event_group;

/** Binary semaphore used to serialize BLE host initialization. */
extern SemaphoreHandle_t  ble_sync_semaphore;

/** Orchestrator event queue (depth ORCHESTRATOR_QUEUE_DEPTH). */
extern QueueHandle_t      s_orchestrator_event_queue;

/* ── Shared Orchestrator State Variables ────────────────────────────────── */

extern bool               s_is_muted;
extern bool               s_arrival_context_sent;
extern bool               s_alert_dispatch_pending;
extern uint32_t           s_alert_timestamp_ms;
extern float              s_alert_corr_drop;
extern webrtc_session_mode_t s_pending_webrtc_mode;
extern webrtc_session_mode_t s_ignition_webrtc_mode;
extern webrtc_session_mode_t s_active_webrtc_mode;
extern bool               s_ble_release_to_sleep;
extern uint32_t           s_vigilante_active_started_ms;
extern uint32_t           s_vigilante_resting_since_ms;
extern bool               s_vigilante_reinforcement_sent;
extern volatile uint32_t  s_sleep_csi_generation;
extern uint32_t           s_sleep_motion_allowed_ms;
extern uint32_t           s_webrtc_stop_started_ms;

/** Task handle for the alert dispatch task (reset by reset_alert_context). */
extern TaskHandle_t       s_alert_dispatch_task_handle;

/** Task handle for the vigilante reinforcement task (reset by reset_vigilante_runtime_context). */
extern TaskHandle_t       s_vigilante_reinforcement_task_handle;

/* ── Helper Function Declarations ───────────────────────────────────────── */

/** Returns the current monotonic time in milliseconds. */
uint32_t    orchestrator_now_ms(void);

/**
 * @brief Create a FreeRTOS task, optionally placing its stack in PSRAM
 *        when the build configuration permits external-memory task stacks.
 */
BaseType_t  orchestrator_create_external_stack_task(TaskFunction_t task_fn,
                                                    const char    *name,
                                                    uint32_t       stack_depth,
                                                    void          *param,
                                                    UBaseType_t    priority,
                                                    TaskHandle_t  *task_handle);

/** Reset alert context variables to idle defaults. */
void        reset_alert_context(void);

/** Reset Vigilante-mode runtime context to idle defaults. */
void        reset_vigilante_runtime_context(void);

/** Returns true when the active WebRTC session is in Vigilante mode. */
bool        orchestrator_is_vigilante_active(void);

/** Cancels any pending CSI cooldown (increments generation counter). */
void        orchestrator_cancel_sleep_csi_cooldown(void);

/** Returns true while the CSI motion-allowed window has not yet opened. */
bool        orchestrator_sleep_cooldown_active(void);

/** Log a heap snapshot tagged with @p stage. */
void        orchestrator_log_heap_snapshot(const char *stage);

/**
 * @brief Ensure the LCD UI is initialized and Dr. Simi canvas is present.
 * @return ESP_OK if UI is ready, ESP_FAIL otherwise.
 */
esp_err_t   orchestrator_ensure_ui_ready(const char *reason);

/**
 * @brief Ensure the audio/media runtime (codec + media pipeline) is ready.
 * @return ESP_OK if audio is ready, ESP_FAIL otherwise.
 */
esp_err_t   orchestrator_ensure_audio_runtime_ready(void);

/**
 * @brief Show a phase banner on the LCD display (title + subtitle + color).
 *
 * Calls orchestrator_ensure_ui_ready() internally; safe to call in any state.
 */
void        orchestrator_show_phase(const char *reason,
                                    const char *title,
                                    const char *subtitle,
                                    uint16_t    color);

/** Render the Vigilante alert visual (Dr. Simi alert or text fallback). */
void        orchestrator_show_vigilante_alert_visual(void);

/**
 * @brief Returns true if the active session has been idle for longer than
 *        AUTO_SLEEP_TIMEOUT_MS and the WebRTC interface is not busy.
 */
bool        orchestrator_active_idle_expired(void);

/** Convert orchestrator_state_t to a human-readable C string. */
const char *orchestrator_state_name(orchestrator_state_t state);

/** Convert orchestrator_event_t to a human-readable C string. */
const char *orchestrator_event_name(orchestrator_event_t event);

/* ── Event Posting Functions ────────────────────────────────────────────── */

void orchestrator_post_event(orchestrator_event_t event);
void orchestrator_post_fatal_error(void);
void orchestrator_post_mute_state(bool is_muted);
bool orchestrator_get_mute_state(void);
void orchestrator_post_motion_detected(uint32_t timestamp_ms, float corr_drop);

/**
 * @brief Post an alert dispatch result event (COMPLETE or FAILED) to the
 *        orchestrator queue, carrying back the alert context for matching.
 */
void orchestrator_post_alert_dispatch_result(orchestrator_event_t event,
                                             uint32_t             timestamp_ms,
                                             float                corr_drop);

/** Log an ignored event (event not handled in the current state). */
void        orchestrator_ignore_event(orchestrator_state_t state,
                                      orchestrator_event_t event);

/**
 * @brief Returns true if the alert result message matches the pending
 *        alert context (timestamp + corr_drop).
 */
bool        orchestrator_alert_result_matches(const orchestrator_event_msg_t *msg);
