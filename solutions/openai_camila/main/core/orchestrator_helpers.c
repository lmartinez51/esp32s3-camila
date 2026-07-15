/**
 * @file orchestrator_helpers.c
 * @brief Shared state, types, and helper implementations for the orchestrator.
 *
 * Holds all cross-module orchestrator variables and the helper functions
 * that implement the common logic used by the FSM, async tasks, and
 * Vigilante monitor: heap logging, UI/audio readiness checks, event posting,
 * and state/event name conversion for logging.
 */

#include "orchestrator_helpers.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

#include "app_events.h"
#include "wifi_session_state.h"
#include "webrtc.h"  /* webrtc_get_last_activity_ms, webrtc_realtime_is_busy */
#include "ui.h"
#include "camila_lvgl_ui.h"
#include "webrtc.h"
#include "media_sys.h"
#include "codec_init.h"
#include "mute_handler.h"
#include "common.h"

static const char *TAG = "MAIN";

/* ── Shared Global Primitives ───────────────────────────────────────────── */

EventGroupHandle_t app_startup_event_group          = NULL;
SemaphoreHandle_t  ble_sync_semaphore               = NULL;
QueueHandle_t      s_orchestrator_event_queue       = NULL;

/* ── Shared Orchestrator State ──────────────────────────────────────────── */

bool               s_is_muted                       = false;
bool               s_arrival_context_sent           = false;
bool               s_alert_dispatch_pending         = false;
uint32_t           s_alert_timestamp_ms             = 0;
float              s_alert_corr_drop                = 0.0f;
webrtc_session_mode_t s_pending_webrtc_mode         = WEBRTC_SESSION_MODE_FRIENDLY;
webrtc_session_mode_t s_ignition_webrtc_mode        = WEBRTC_SESSION_MODE_FRIENDLY;
webrtc_session_mode_t s_active_webrtc_mode          = WEBRTC_SESSION_MODE_FRIENDLY;

/** Phase 1: Populated from NVS in app_main() before the orchestrator task starts. */
boot_operation_mode_t g_boot_operation_mode          = BOOT_MODE_DIRECTO;

bool               s_ble_release_to_sleep           = false;
uint32_t           s_vigilante_active_started_ms    = 0;
uint32_t           s_vigilante_resting_since_ms     = 0;
bool               s_vigilante_reinforcement_sent   = false;
volatile uint32_t  s_sleep_csi_generation           = 0;
uint32_t           s_sleep_motion_allowed_ms        = 0;
uint32_t           s_webrtc_stop_started_ms         = 0;
TaskHandle_t       s_alert_dispatch_task_handle     = NULL;
TaskHandle_t       s_vigilante_reinforcement_task_handle = NULL;

/* ── Time ───────────────────────────────────────────────────────────────── */

uint32_t orchestrator_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ── Task Creation Helper ───────────────────────────────────────────────── */

BaseType_t orchestrator_create_external_stack_task(TaskFunction_t task_fn,
                                                   const char    *name,
                                                   uint32_t       stack_depth,
                                                   void          *param,
                                                   UBaseType_t    priority,
                                                   TaskHandle_t  *task_handle)
{
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    return xTaskCreatePinnedToCoreWithCaps(task_fn,
                                           name,
                                           stack_depth,
                                           param,
                                           priority,
                                           task_handle,
                                           tskNO_AFFINITY,
                                           ORCHESTRATOR_EXTERNAL_STACK_CAPS);
#else
    return xTaskCreate(task_fn, name, stack_depth, param, priority, task_handle);
#endif
}

/* ── Context Reset Helpers ──────────────────────────────────────────────── */

void reset_alert_context(void)
{
    s_alert_timestamp_ms             = 0;
    s_alert_corr_drop                = 0.0f;
    s_alert_dispatch_pending         = false;
    s_alert_dispatch_task_handle     = NULL;
    s_pending_webrtc_mode            = WEBRTC_SESSION_MODE_FRIENDLY;
}

void reset_vigilante_runtime_context(void)
{
    s_active_webrtc_mode                 = WEBRTC_SESSION_MODE_FRIENDLY;
    s_ignition_webrtc_mode               = WEBRTC_SESSION_MODE_FRIENDLY;
    s_vigilante_active_started_ms        = 0;
    s_vigilante_resting_since_ms         = 0;
    s_vigilante_reinforcement_sent       = false;
    s_vigilante_reinforcement_task_handle = NULL;
}

/* ── Vigilante State Query ──────────────────────────────────────────────── */

bool orchestrator_is_vigilante_active(void)
{
    return s_active_webrtc_mode == WEBRTC_SESSION_MODE_VIGILANTE;
}

/* ── CSI Cooldown Helpers ───────────────────────────────────────────────── */

void orchestrator_cancel_sleep_csi_cooldown(void)
{
    s_sleep_csi_generation++;
    s_sleep_motion_allowed_ms = 0;
}

bool orchestrator_sleep_cooldown_active(void)
{
    return s_sleep_motion_allowed_ms != 0 &&
           (int32_t)(orchestrator_now_ms() - s_sleep_motion_allowed_ms) < 0;
}

/* ── Heap Snapshot ──────────────────────────────────────────────────────── */

/**
 * @brief Take and log a heap memory snapshot.
 *
 * Captures current internal and PSRAM memory usage.  "telemetry:periodic"
 * is logged at DEBUG level; all other stages use WARN so they stand out.
 *
 * @param stage A label indicating the context where the snapshot was taken.
 */
void orchestrator_log_heap_snapshot(const char *stage)
{
    const char *label = stage ? stage : "unknown";

    const size_t internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    const size_t dma_free    = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    const size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    if (strcmp(label, "telemetry:periodic") == 0)
    {
        ESP_LOGD(TAG,
                 "[HEAP] %s | INTERNAL free=%zu min=%zu largest=%zu | DMA free=%zu largest=%zu | PSRAM free=%zu min=%zu largest=%zu",
                 label,
                 internal_free, internal_min, internal_largest,
                 dma_free, dma_largest,
                 psram_free, psram_min, psram_largest);
    }
    else
    {
        ESP_LOGW(TAG,
                 "[HEAP] %s | INTERNAL free=%zu min=%zu largest=%zu | DMA free=%zu largest=%zu | PSRAM free=%zu min=%zu largest=%zu",
                 label,
                 internal_free, internal_min, internal_largest,
                 dma_free, dma_largest,
                 psram_free, psram_min, psram_largest);
    }
}

/* ── UI / Audio Readiness ───────────────────────────────────────────────── */

esp_err_t orchestrator_ensure_ui_ready(const char *reason)
{
    if (ui_is_initialized())
    {

        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restoring LCD UI after BLE handoff: %s", reason ? reason : "");
    orchestrator_log_heap_snapshot("ui_restore:before");
    esp_err_t err = ui_init();
    orchestrator_log_heap_snapshot((err == ESP_OK) ? "ui_restore:after" : "ui_restore:failed");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD UI restore failed: %s", esp_err_to_name(err));
    }
    return err;
}

void orchestrator_show_phase(const char *reason,
                             const char *title,
                             const char *subtitle,
                             uint16_t    color)
{
    if (orchestrator_ensure_ui_ready(reason) == ESP_OK)
    {
        display_system_phase_message(title, subtitle, color);
    }
}

void orchestrator_show_vigilante_alert_visual(void)
{
    esp_err_t err = orchestrator_ensure_ui_ready("dispatching_alert");
    if (err != ESP_OK) {
        return;
    }


    display_intruder_alert_message();
}

/**
 * @brief Ensure that the audio runtime is ready for use.
 *
 * Checks if the audio runtime is already initialized. If not, initializes it
 * via media_sys_buildup() and verifies readiness. Must be called before any
 * audio-related features to prevent uninitialized runtime errors.
 *
 * @return ESP_OK if ready, ESP_FAIL if initialization failed.
 */
esp_err_t orchestrator_ensure_audio_runtime_ready(void)
{
    if (media_sys_is_ready()) {
        return ESP_OK;
    }

    orchestrator_log_heap_snapshot("audio_runtime:before");
    unpark_i2s();
    int media_ret = media_sys_buildup();
    if (media_ret != 0) {
        ESP_LOGE(TAG, "Audio/media runtime init failed: %d", media_ret);
        orchestrator_log_heap_snapshot("audio_runtime:media_failed");
        return ESP_FAIL;
    }

    orchestrator_log_heap_snapshot("audio_runtime:media_ready");
    if (!media_sys_is_ready()) {
        ESP_LOGE(TAG, "Audio/media runtime reported success but is not ready");
        orchestrator_log_heap_snapshot("audio_runtime:not_ready");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ── Active Idle Check ──────────────────────────────────────────────────── */

/**
 * @brief Check if the active session idle timeout has expired.
 *
 * Returns true only when the session has been idle for the entire
 * AUTO_SLEEP_TIMEOUT_MS duration and the WebRTC interface is not busy.
 */
bool orchestrator_active_idle_expired(void)
{
    if (s_is_muted) {
        return false;
    }

    uint32_t last_activity_ms = webrtc_get_last_activity_ms();
    if (last_activity_ms == 0 || webrtc_realtime_is_busy()) {
        return false;
    }

    return (uint32_t)(orchestrator_now_ms() - last_activity_ms) >= AUTO_SLEEP_TIMEOUT_MS;
}

/* ── Name Conversion for Logging ────────────────────────────────────────── */

/**
 * @brief Convert orchestrator_state_t to a human-readable string.
 */
const char *orchestrator_state_name(orchestrator_state_t state)
{
    switch (state)
    {
    case STATE_WAIT_WIFI:           return "STATE_WAIT_WIFI";
    case STATE_SLEEP:               return "STATE_SLEEP";
    case STATE_PREPARING_BLE:       return "STATE_PREPARING_BLE";
    case STATE_VALIDATING_IDENTITY: return "STATE_VALIDATING_IDENTITY";
    case STATE_RELEASING_BLE:       return "STATE_RELEASING_BLE";
    case STATE_DISPATCHING_ALERT:   return "STATE_DISPATCHING_ALERT";
    case STATE_IGNITING:            return "STATE_IGNITING";
    case STATE_ACTIVE:              return "STATE_ACTIVE";
    case STATE_AUTO_SLEEPING:       return "STATE_AUTO_SLEEPING";
    case STATE_STOPPING_WEBRTC:     return "STATE_STOPPING_WEBRTC";
    case STATE_TEARING_DOWN_UI_FOR_SCAN: return "STATE_TEARING_DOWN_UI_FOR_SCAN";
    case STATE_AUTO_ARM_BLE_SCANNING: return "STATE_AUTO_ARM_BLE_SCANNING";
    case STATE_FATAL_ERROR:         return "STATE_FATAL_ERROR";
    default:                        return "STATE_UNKNOWN";
    }
}

/**
 * @brief Convert orchestrator_event_t to a human-readable string.
 */
const char *orchestrator_event_name(orchestrator_event_t event)
{
    switch (event)
    {
    case ORCH_EVENT_WIFI_CONNECTED:          return "ORCH_EVENT_WIFI_CONNECTED";
    case ORCH_EVENT_WIFI_DISCONNECTED:       return "ORCH_EVENT_WIFI_DISCONNECTED";
    case ORCH_EVENT_MOTION_DETECTED:         return "ORCH_EVENT_MOTION_DETECTED";
    case ORCH_EVENT_BLE_READY:               return "ORCH_EVENT_BLE_READY";
    case ORCH_EVENT_BLE_BUSY:                return "ORCH_EVENT_BLE_BUSY";
    case ORCH_EVENT_IDENTITY_PRESENT:        return "ORCH_EVENT_IDENTITY_PRESENT";
    case ORCH_EVENT_IDENTITY_REJECTED:       return "ORCH_EVENT_IDENTITY_REJECTED";
    case ORCH_EVENT_BLE_RELEASE_COMPLETE:    return "ORCH_EVENT_BLE_RELEASE_COMPLETE";
    case ORCH_EVENT_BLE_RELEASE_FAILED:      return "ORCH_EVENT_BLE_RELEASE_FAILED";
    case ORCH_EVENT_WEBRTC_CONNECTED:        return "ORCH_EVENT_WEBRTC_CONNECTED";
    case ORCH_EVENT_WEBRTC_DISCONNECTED:     return "ORCH_EVENT_WEBRTC_DISCONNECTED";
    case ORCH_EVENT_WEBRTC_API_ERROR:        return "ORCH_EVENT_WEBRTC_API_ERROR";
    case ORCH_EVENT_WEBRTC_STOPPED:          return "ORCH_EVENT_WEBRTC_STOPPED";
    case ORCH_EVENT_AUTO_SLEEP_TIMEOUT:      return "ORCH_EVENT_AUTO_SLEEP_TIMEOUT";
    case ORCH_EVENT_ALERT_DISPATCH_COMPLETE: return "ORCH_EVENT_ALERT_DISPATCH_COMPLETE";
    case ORCH_EVENT_ALERT_DISPATCH_FAILED:   return "ORCH_EVENT_ALERT_DISPATCH_FAILED";
    case ORCH_EVENT_VIGILANTE_ROOM_VACATED:  return "ORCH_EVENT_VIGILANTE_ROOM_VACATED";
    case ORCH_EVENT_VIGILANTE_TIMEOUT:       return "ORCH_EVENT_VIGILANTE_TIMEOUT";
    case ORCH_EVENT_MIC_MUTED:               return "ORCH_EVENT_MIC_MUTED";
    case ORCH_EVENT_MIC_UNMUTED:             return "ORCH_EVENT_MIC_UNMUTED";
    case ORCH_EVENT_IDLE_ALERT_START:        return "ORCH_EVENT_IDLE_ALERT_START";
    case ORCH_EVENT_IDLE_ALERT_END:          return "ORCH_EVENT_IDLE_ALERT_END";
    case ORCH_EVENT_RAM_FREED:               return "ORCH_EVENT_RAM_FREED";
    case ORCH_EVENT_FATAL_ERROR:             return "ORCH_EVENT_FATAL_ERROR";
    default:                                 return "ORCH_EVENT_UNKNOWN";
    }
}

/* ── Event Posting ──────────────────────────────────────────────────────── */

/**
 * @brief Post an event to the orchestrator queue with logging.
 *
 * Drops the event (with a warning) if the queue is NULL or full.
 */
void orchestrator_post_event(orchestrator_event_t event)
{
    orchestrator_event_msg_t msg = { .type = event };

    if (s_orchestrator_event_queue == NULL) {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping event=%d", event);
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(event));
    }
}

void orchestrator_post_fatal_error(void)
{
    orchestrator_event_msg_t msg = { .type = ORCH_EVENT_FATAL_ERROR };

    if (s_orchestrator_event_queue == NULL) {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping fatal error");
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping fatal error");
    }
}

void orchestrator_post_mute_state(bool is_muted)
{
    orchestrator_event_t evt = is_muted ? ORCH_EVENT_MIC_MUTED : ORCH_EVENT_MIC_UNMUTED;
    orchestrator_post_event(evt);
}

bool orchestrator_get_mute_state(void)
{
    return s_is_muted;
}

void orchestrator_post_motion_detected(uint32_t timestamp_ms, float corr_drop)
{
    orchestrator_event_msg_t msg = {
        .type         = ORCH_EVENT_MOTION_DETECTED,
        .timestamp_ms = timestamp_ms,
        .corr_drop    = corr_drop,
    };

    if (s_orchestrator_event_queue == NULL) {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping motion event");
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s",
                 orchestrator_event_name(msg.type));
    }
}

void orchestrator_post_alert_dispatch_result(orchestrator_event_t event,
                                             uint32_t             timestamp_ms,
                                             float                corr_drop)
{
    if (s_orchestrator_event_queue == NULL) {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping event=%d", event);
        return;
    }

    orchestrator_event_msg_t msg = {
        .type         = event,
        .timestamp_ms = timestamp_ms,
        .corr_drop    = corr_drop,
    };

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(event));
    }
}

/* ── FSM Logging Helpers ────────────────────────────────────────────────── */

void orchestrator_ignore_event(orchestrator_state_t state, orchestrator_event_t event)
{
    ESP_LOGI(TAG, "Orchestrator ignoring %s in %s",
             orchestrator_event_name(event),
             orchestrator_state_name(state));
}

bool orchestrator_alert_result_matches(const orchestrator_event_msg_t *msg)
{
    return msg &&
           msg->timestamp_ms == s_alert_timestamp_ms &&
           msg->corr_drop    == s_alert_corr_drop;
}
