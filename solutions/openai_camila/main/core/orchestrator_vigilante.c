/**
 * @file orchestrator_vigilante.c
 * @brief Vigilante-mode presence monitor implementation.
 *
 * Implements the periodic polling logic that converts raw Edge-Impulse
 * inference results into high-level orchestrator events:
 *   - ORCH_EVENT_VIGILANTE_TIMEOUT  (max session time exceeded)
 *   - ORCH_EVENT_VIGILANTE_ROOM_VACATED (room confirmed empty for N ms)
 *
 * Also dispatches the reinforcement alert when the motion has persisted
 * for VIGILANTE_REINFORCEMENT_DELAY_MS without a resolution.
 */

#include "orchestrator_vigilante.h"
#include "orchestrator_helpers.h"
#include "orchestrator_tasks.h"

#include "esp_log.h"
#include "ei_inference.h"
#include "webrtc.h"

static const char *TAG = "MAIN";

/* ── Public API ─────────────────────────────────────────────────────────── */

void orchestrator_note_vigilante_motion_detected(const orchestrator_event_msg_t *msg)
{
    if (!orchestrator_is_vigilante_active()) {
        return;
    }

    if (s_vigilante_resting_since_ms != 0) {
        ESP_LOGI(TAG,
                 "Vigilante resting window reset by motion event: corr_drop=%.4f timestamp=%lu",
                 msg ? msg->corr_drop : 0.0f,
                 (unsigned long)(msg ? msg->timestamp_ms : 0));
    }
    s_vigilante_resting_since_ms = 0;
}

bool orchestrator_poll_vigilante_monitor(orchestrator_event_msg_t *out_msg)
{
    if (!out_msg || !orchestrator_is_vigilante_active()) {
        return false;
    }

    const uint32_t now_ms = orchestrator_now_ms();
    if (s_vigilante_active_started_ms == 0) {
        s_vigilante_active_started_ms = now_ms;
    }

    /* ── Hard timeout ───────────────────────────────────────────────── */
    if ((uint32_t)(now_ms - s_vigilante_active_started_ms) >= VIGILANTE_ACTIVE_TIMEOUT_MS) {
        out_msg->type = ORCH_EVENT_VIGILANTE_TIMEOUT;
        return true;
    }

    /* ── Edge-Impulse inference status ─────────────────────────────── */
    ei_inference_status_t status = {0};
    ei_inference_get_status(&status);
    const bool status_fresh = status.valid &&
                              (uint32_t)(now_ms - status.updated_ms) <= VIGILANTE_STATUS_STALE_MS;

    /* ── Reinforcement alert ────────────────────────────────────────── */
    if (!s_vigilante_reinforcement_sent &&
        (uint32_t)(now_ms - s_vigilante_active_started_ms) >= VIGILANTE_REINFORCEMENT_DELAY_MS)
    {
        s_vigilante_reinforcement_sent = true;
        if (status_fresh && status.motion_active) {
            ESP_LOGW(TAG,
                     "Vigilante motion persists after %d ms; dispatching reinforcement alert",
                     VIGILANTE_REINFORCEMENT_DELAY_MS);
            esp_err_t err = orchestrator_start_alert_dispatch(now_ms,
                                                              status.corr_drop,
                                                              true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start reinforcement alert: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Vigilante reinforcement skipped; motion is not active");
        }
    }

    /* ── Room-vacated detection ─────────────────────────────────────── */
    if (status_fresh && status.resting)
    {
        if (s_vigilante_resting_since_ms == 0) {
            s_vigilante_resting_since_ms = now_ms;
        } else if ((uint32_t)(now_ms - s_vigilante_resting_since_ms) >= VIGILANTE_VACATED_CONFIRM_MS) {
            if (webrtc_realtime_is_busy()) {
                ESP_LOGI(TAG,
                         "Vigilante room-vacated candidate suppressed while WebRTC audio/session is busy");
                s_vigilante_resting_since_ms = now_ms;
                return false;
            }

            out_msg->type         = ORCH_EVENT_VIGILANTE_ROOM_VACATED;
            out_msg->timestamp_ms = status.updated_ms;
            out_msg->corr_drop    = status.corr_drop;
            return true;
        }
    }
    else
    {
        s_vigilante_resting_since_ms = 0;
    }

    return false;
}
