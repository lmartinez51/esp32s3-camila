/**
 * @file orchestrator_fsm.c
 * @brief Orchestrator finite-state machine: state transitions and main task.
 *
 * Contains:
 *   - orchestrator_enter_state(): all state entry actions (CSI/radar
 *     arming, BLE/WebRTC lifecycle, UI updates).
 *   - app_startup_orchestrator_task(): the main orchestrator loop that
 *     dequeues events and drives the state machine.
 *
 * States handled:
 *   STATE_WAIT_WIFI → STATE_SLEEP → STATE_PREPARING_BLE →
 *   STATE_VALIDATING_IDENTITY → STATE_RELEASING_BLE →
 *   STATE_DISPATCHING_ALERT → STATE_IGNITING → STATE_ACTIVE →
 *   STATE_AUTO_SLEEPING → STATE_STOPPING_WEBRTC → STATE_FATAL_ERROR
 */

#include "orchestrator_fsm.h"
#include "orchestrator_helpers.h"
#include "orchestrator_tasks.h"
#include "orchestrator_vigilante.h"
// Removed sensor_dock.h

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "app_events.h"
#include "wifi_session_state.h"
#include "webrtc.h"
#include "ui.h"
#include "camila_lvgl_ui.h"
#include "camila_lvgl_ui.h"
#include "media_sys.h"
#include "mute_handler.h"
#include "ble_device_callbacks.h"
#include "ble_device_control.h"
#include "csi_handler.h"
// Removed radar.h
#include "common.h"
#include "esp_now_beacon.h"

static const char *TAG = "MAIN";

/* ── Phase 3: RAM Guillotine Timer & State ──────────────────────────────── */

static esp_timer_handle_t s_ram_guillotine_timer = NULL;
static bool s_teardown_ui_pending = false;

static void ram_guillotine_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "RAM Guillotine Timer Expired! Posting ORCH_EVENT_RAM_FREED");
    orchestrator_post_event(ORCH_EVENT_RAM_FREED);
}

/* ── Fatal Error Timer ──────────────────────────────────────────────────── */

static void fatal_error_timer_cb(void *arg)
{
    ESP_LOGE(TAG, "Fatal error 15-minute timeout reached. Rebooting system now.");
    esp_restart();
}

/* ── State Entry Actions ────────────────────────────────────────────────── */

/**
 * @brief Enter a new state in the orchestrator state machine.
 *
 * Performs all entry actions for the target state. Each state entry:
 *   - Stops sensors / subsystems no longer needed.
 *   - Updates the LCD to show the current phase.
 *   - Kicks off asynchronous operations (BLE, WebRTC, alerts).
 *
 * Idempotent when *state == next_state.
 *
 * @param state      Pointer to the current state (updated in-place).
 * @param next_state Target state to enter.
 */
void orchestrator_enter_state(orchestrator_state_t *state,
                              orchestrator_state_t  next_state)
{
    if (state == NULL) {
        return;
    }

    if (*state == next_state) {
        ESP_LOGI(TAG, "Orchestrator remains in %s", orchestrator_state_name(next_state));
        return;
    }

    *state = next_state;

    switch (next_state)
    {
    case STATE_WAIT_WIFI:
        ESP_LOGI(TAG, "STATE_WAIT_WIFI: stopping CSI, WebRTC, and BLE smart/control tasks.");
        orchestrator_cancel_sleep_csi_cooldown();
        reset_alert_context();
        reset_vigilante_runtime_context();
        s_ble_release_to_sleep = false;
        s_is_muted             = false;
        // radar_hal_disable() removed
        csi_handler_stop();
        ui_deinit_keep_last_frame();
        stop_webrtc();
        ble_device_stop_smart_task();
        ble_device_control_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WIFI_CONNECTED_BIT | WEBRTC_CONNECTED_BIT |
                                 WEBRTC_DISCONNECTED_BIT | WEBRTC_API_ERROR_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        display_disconnected_message();
        break;

    case STATE_SLEEP:
    {
        ESP_LOGI(TAG, "STATE_SLEEP: WiFi is up; WebRTC remains off. Starting CSI motion sensing.");
        esp_now_beacon_init();

        /* ── Phase 1: Arm NVS for next boot if a Vigilante session just ended ──
         * s_active_webrtc_mode retains WEBRTC_SESSION_MODE_VIGILANTE until the
         * next successful ignition. Writing CENTINELA here ensures that if the
         * device is power-cycled while re-armed in STATE_SLEEP, the next boot
         * starts in the armed path automatically.
         * The write is idempotent — if CENTINELA is already stored, no harm.   */
        if (s_active_webrtc_mode == WEBRTC_SESSION_MODE_VIGILANTE) {
            ESP_LOGW(TAG, "STATE_SLEEP: Sesión Vigilante concluida. "
                          "Escribiendo CENTINELA en NVS para el próximo arranque.");
            nvs_set_operation_mode(BOOT_MODE_CENTINELA);
        }
        /* ────────────────────────────────────────────────────────────────── */

        media_sys_set_vigilante_mute(false);
        reset_alert_context();
        reset_vigilante_runtime_context();
        s_ble_release_to_sleep    = false;
        s_is_muted                = false;
        s_webrtc_stop_started_ms  = 0;
        // radar_hal_disable() removed
        csi_handler_stop();
        ui_deinit_keep_last_frame();
        orchestrator_show_phase("wifi_ready", "WiFi ready", "Standing by", COLOR_GREEN_BGR565);
        esp_err_t sleep_ble_err = ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        if (sleep_ble_err != ESP_OK) {
            ESP_LOGE(TAG, "STATE_SLEEP: BLE full release failed; continuing CSI recovery path: %s",
                     esp_err_to_name(sleep_ble_err));
            orchestrator_log_heap_snapshot("sleep:ble_release_failed");
        }
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT | WIFI_DISCONNECTED_BIT);
        ESP_LOGI(TAG, "STATE_SLEEP: waiting %d ms before enabling CSI.", SLEEP_CSI_COOLDOWN_MS);
        orchestrator_schedule_sleep_csi_start();
        break;
    }

    case STATE_PREPARING_BLE:
    {
        ESP_LOGI(TAG, "STATE_PREPARING_BLE: pausing CSI and cold-booting BLE for identity validation.");
        orchestrator_cancel_sleep_csi_cooldown();
        // radar_hal_disable() removed
        csi_handler_stop();
        orchestrator_show_phase("ble_prepare_status", "Checking ID", "Stay close", COLOR_YELLOW_BGR565);
        esp_err_t ui_deinit_err = ui_deinit_keep_last_frame();
        if (ui_deinit_err != ESP_OK) {
            ESP_LOGW(TAG, "STATE_PREPARING_BLE: LCD release returned %s",
                     esp_err_to_name(ui_deinit_err));
        }
        orchestrator_log_heap_snapshot("ble_prepare:after_ui_release");
        orchestrator_start_ble_prepare();
        break;
    }

    case STATE_VALIDATING_IDENTITY:
        ESP_LOGI(TAG, "STATE_VALIDATING_IDENTITY: BLE ready; starting identity validation scan.");
        orchestrator_cancel_sleep_csi_cooldown();
        // radar_hal_disable() removed
        csi_handler_stop();
        
        camila_ui_update_state(UI_STATE_BLE_SCAN, "CENTINELA", "Scanning for owner...");
        
        orchestrator_start_identity_validation();
        break;

    case STATE_RELEASING_BLE:
        ESP_LOGI(TAG, "STATE_RELEASING_BLE: releasing NimBLE before audio ignition.");
        orchestrator_cancel_sleep_csi_cooldown();
        // radar_hal_disable() removed
        csi_handler_stop();
        orchestrator_start_ble_release();
        break;

    case STATE_DISPATCHING_ALERT:
    {
        ESP_LOGW(TAG, "STATE_DISPATCHING_ALERT: dispatching emergency alert after BLE release.");
        orchestrator_cancel_sleep_csi_cooldown();
        // radar_hal_disable() removed
        csi_handler_stop();
        orchestrator_show_vigilante_alert_visual();
        if (!s_alert_dispatch_pending) {
            ESP_LOGW(TAG, "STATE_DISPATCHING_ALERT entered without pending alert context");
            orchestrator_post_alert_dispatch_result(ORCH_EVENT_ALERT_DISPATCH_FAILED,
                                                    s_alert_timestamp_ms,
                                                    s_alert_corr_drop);
            break;
        }

        esp_err_t alert_err = orchestrator_start_alert_dispatch(s_alert_timestamp_ms,
                                                                s_alert_corr_drop,
                                                                false);
        if (alert_err != ESP_OK) {
            ESP_LOGE(TAG, "STATE_DISPATCHING_ALERT: failed to start alert task: %s",
                     esp_err_to_name(alert_err));
            orchestrator_post_alert_dispatch_result(ORCH_EVENT_ALERT_DISPATCH_FAILED,
                                                    s_alert_timestamp_ms,
                                                    s_alert_corr_drop);
        }
        break;
    }

    case STATE_IGNITING:
    {
        ESP_LOGI(TAG, "STATE_IGNITING: initializing audio runtime and starting WebRTC.");
        esp_now_beacon_init();
        orchestrator_cancel_sleep_csi_cooldown();
        // radar_hal_disable() removed
        csi_handler_stop();
        s_arrival_context_sent = false;

        camila_ui_update_state(UI_STATE_SUCCESS, "SYSTEM READY", "Igniting audio pipeline...");

        /* Reset persistent session logic to cold-boot defaults to guarantee sync with UI */
        s_is_muted = false;
        mute_handler_stop_idle_timer();
        webrtc_session_mode_t ignition_mode = s_ignition_webrtc_mode;
        s_active_webrtc_mode = ignition_mode;

        orchestrator_log_heap_snapshot("igniting:before_audio");

        /* Ensure UI is ready and show Welcome Screen before audio loads */
        if (orchestrator_ensure_ui_ready("igniting") == ESP_OK) {
            if (ignition_mode != WEBRTC_SESSION_MODE_VIGILANTE) {
                display_welcome_identity(ble_identity_get_last_validated_name());
            }
        }

        esp_err_t audio_err = orchestrator_ensure_audio_runtime_ready();
        if (audio_err != ESP_OK || !media_sys_is_ready()) {
            ESP_LOGE(TAG, "STATE_IGNITING: audio runtime not ready; aborting WebRTC start");
            orchestrator_log_heap_snapshot("igniting:audio_failed");
            orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
            break;
        }
        orchestrator_log_heap_snapshot("igniting:audio_ready");

        int ret = start_webrtc(ignition_mode);
        s_ignition_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
        if (ret != 0) {
            EventBits_t bits = xEventGroupGetBits(app_startup_event_group);
            orchestrator_post_event((bits & WEBRTC_API_ERROR_BIT)
                                        ? ORCH_EVENT_WEBRTC_API_ERROR
                                        : ORCH_EVENT_WEBRTC_DISCONNECTED);
        }
        break;
    }

    case STATE_ACTIVE:
        ESP_LOGI(TAG, "STATE_ACTIVE: WebRTC active; injecting arrival context.");
        orchestrator_cancel_sleep_csi_cooldown();
        
        camila_ui_show_avatar();
        
        if (s_is_muted) {
            ESP_LOGI(TAG, "STATE_ACTIVE: Re-applying persisted mute state.");
            ui_show_status_message("Muted / Dozing", COLOR_RED_BGR565);
        }
        if (orchestrator_is_vigilante_active()) {
            ESP_LOGW(TAG, "STATE_ACTIVE: Vigilante Mode active; starting CSI threat monitor.");
            s_vigilante_active_started_ms = orchestrator_now_ms();
            s_vigilante_resting_since_ms  = 0;
            s_vigilante_reinforcement_sent = false;
            esp_err_t csi_err = csi_handler_start();
            if (csi_err != ESP_OK) {
                ESP_LOGE(TAG, "STATE_ACTIVE: failed to start Vigilante CSI monitor: %s",
                         esp_err_to_name(csi_err));
            }
        } else {
            // radar_hal_disable() removed
            csi_handler_stop();
        }
        if (!s_arrival_context_sent) {
            if (webrtc_inject_arrival_context() == 0) {
                s_arrival_context_sent = true;
            } else {
                ESP_LOGW(TAG, "Arrival context injection failed; will not retry in this active session");
                s_arrival_context_sent = true;
            }
        }
        break;

    case STATE_AUTO_SLEEPING:
        ESP_LOGI(TAG, "STATE_AUTO_SLEEPING: preparing deterministic WebRTC shutdown.");
        orchestrator_cancel_sleep_csi_cooldown();
        s_arrival_context_sent = false;
        // radar_hal_disable() removed
        csi_handler_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT);
        break;

    case STATE_STOPPING_WEBRTC:
        ESP_LOGI(TAG, "STATE_STOPPING_WEBRTC: waiting for explicit WebRTC stopped event.");
        media_sys_set_vigilante_mute(false);
        orchestrator_cancel_sleep_csi_cooldown();
        s_arrival_context_sent = false;
        // radar_hal_disable() removed
        csi_handler_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT);
        orchestrator_start_webrtc_stop();
        break;

    case STATE_TEARING_DOWN_UI_FOR_SCAN:
    {
        ESP_LOGW(TAG, "STATE_TEARING_DOWN_UI_FOR_SCAN: tearing down UI to free RAM for BLE Scan");
        // radar_hal_disable() removed
        csi_handler_stop();
        ui_deinit_keep_last_frame();
        
        esp_timer_create_args_t timer_args = {
            .callback = &ram_guillotine_timer_cb,
            .name     = "ram_guillotine_timer"
        };
        if (s_ram_guillotine_timer != NULL) {
            esp_timer_stop(s_ram_guillotine_timer);
            esp_timer_delete(s_ram_guillotine_timer);
            s_ram_guillotine_timer = NULL;
        }
        if (esp_timer_create(&timer_args, &s_ram_guillotine_timer) == ESP_OK) {
            esp_timer_start_once(s_ram_guillotine_timer, 500000ULL); /* 500 ms */
        }
        break;
    }

    case STATE_AUTO_ARM_BLE_SCANNING:
        ESP_LOGI(TAG, "STATE_AUTO_ARM_BLE_SCANNING: Booting BLE stack headlessly for Ghost Scan...");
        orchestrator_start_ble_prepare();
        break;

    default:
        break;
    }
}

/* ── Orchestrator Main Task ─────────────────────────────────────────────── */

/**
 * @brief Orchestrator task — drives the state machine from the event queue.
 *
 * Runs an infinite loop that waits for events from s_orchestrator_event_queue
 * and calls orchestrator_enter_state() to perform transitions. Also handles:
 *   - Periodic auto-sleep idle check (every AUTO_SLEEP_POLL_MS while ACTIVE).
 *   - Periodic Vigilante monitor poll (same interval while ACTIVE + Vigilante).
 *   - Periodic AHT30 temperature read (every 10 min while any state).
 *
 * @param param Unused; pass NULL.
 */
void app_startup_orchestrator_task(void *param)
{
    (void)param;
    orchestrator_state_t     state = STATE_WAIT_WIFI;
    orchestrator_event_msg_t event_msg;

    ESP_LOGI(TAG, "Orchestrator state machine started in %s", orchestrator_state_name(state));

    while (1)
    {
        TickType_t wait_ticks = (state == STATE_ACTIVE || state == STATE_STOPPING_WEBRTC)
                                    ? pdMS_TO_TICKS(AUTO_SLEEP_POLL_MS)
                                    : portMAX_DELAY;

        if (xQueueReceive(s_orchestrator_event_queue, &event_msg, wait_ticks) != pdTRUE)
        {
            memset(&event_msg, 0, sizeof(event_msg));
            if (state == STATE_ACTIVE &&
                     orchestrator_is_vigilante_active() &&
                     orchestrator_poll_vigilante_monitor(&event_msg))
            {
                /* event_msg was filled by the monitor. */
            }
            else if (state == STATE_ACTIVE &&
                     !orchestrator_is_vigilante_active() &&
                     orchestrator_active_idle_expired())
            {
                ESP_LOGI(TAG, "Inactividad de 3 minutos detectada. Ejecutando Auto-Mute en lugar de Auto-Sleep.");
                event_msg.type = ORCH_EVENT_MIC_MUTED;
            }
            else
            {
                // AHT30 polling removed
                continue;
            }
        }

        orchestrator_event_t event = event_msg.type;
        ESP_LOGI(TAG, "Orchestrator event: %s while in %s",
                 orchestrator_event_name(event),
                 orchestrator_state_name(state));

        /* GLOBAL FATAL ERROR INTERCEPT */
        if (event == ORCH_EVENT_FATAL_ERROR && state != STATE_FATAL_ERROR) {
            orchestrator_enter_state(&state, STATE_FATAL_ERROR);
        }

        // AHT30 WebRTC hooks removed

        switch (state)
        {
        case STATE_WAIT_WIFI:
            if (event == ORCH_EVENT_WIFI_CONNECTED) {
                /* Camila Hardware Mode: DIRECTO only. Radar is physically removed. */
                ESP_LOGI(TAG, "Camila Mode: omitiendo STATE_SLEEP (Radar retirado); salto directo a validación BLE.");
                
                orchestrator_enter_state(&state, STATE_PREPARING_BLE);
                /* ────────────────────────────────────────────────────── */
            } else if (event != ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_SLEEP:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_MOTION_DETECTED) {
                if (orchestrator_sleep_cooldown_active()) {
                    ESP_LOGW(TAG, "Ignoring motion event during %d ms STATE_SLEEP cooldown",
                             SLEEP_CSI_COOLDOWN_MS);
                    break;
                }
                s_alert_timestamp_ms      = event_msg.timestamp_ms ? event_msg.timestamp_ms
                                                                    : orchestrator_now_ms();
                s_alert_corr_drop         = event_msg.corr_drop;
                s_alert_dispatch_pending  = false;
                s_pending_webrtc_mode     = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ble_release_to_sleep    = false;
                orchestrator_show_phase("motion_detected", "Motion seen", "Checking ID",
                                        COLOR_YELLOW_BGR565);
                orchestrator_enter_state(&state, STATE_PREPARING_BLE);
            } else if (event != ORCH_EVENT_WIFI_CONNECTED) {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_PREPARING_BLE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_BLE_READY) {
                orchestrator_enter_state(&state, STATE_VALIDATING_IDENTITY);
            } else if (event == ORCH_EVENT_BLE_BUSY) {
                ESP_LOGE(TAG, "BLE preparation failed; returning to sleep without alert dispatch");
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_SLEEP);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_VALIDATING_IDENTITY:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_IDENTITY_PRESENT) {
                /* ── Phase 1: CENTINELA reset on owner validation ───────────
                 * The owner has been authenticated during an armed boot.
                 * Reset NVS to DIRECTO so the NEXT power-cycle starts
                 * in the normal (non-armed) state. Also update the in-memory
                 * flag so subsequent STATE_SLEEP entries do not re-write
                 * CENTINELA for this (now friendly) session.               */
                if (g_boot_operation_mode == BOOT_MODE_CENTINELA) {
                    ESP_LOGI(TAG, "CENTINELA: Propietario validado. "
                                  "Restableciendo NVS a DIRECTO.");
                    nvs_set_operation_mode(BOOT_MODE_DIRECTO);
                    g_boot_operation_mode = BOOT_MODE_DIRECTO;
                }
                /* ────────────────────────────────────────────────────────── */
                media_sys_set_vigilante_mute(false);
                s_alert_dispatch_pending  = false;
                s_pending_webrtc_mode     = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ignition_webrtc_mode    = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ble_release_to_sleep    = false;
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            } else if (event == ORCH_EVENT_IDENTITY_REJECTED) {
                if (s_alert_timestamp_ms == 0) {
                    s_alert_timestamp_ms = orchestrator_now_ms();
                }
                s_alert_dispatch_pending  = true;
                s_pending_webrtc_mode     = WEBRTC_SESSION_MODE_VIGILANTE;
                s_ble_release_to_sleep    = false;
                media_sys_set_vigilante_mute(true);
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            } else if (event == ORCH_EVENT_BLE_BUSY) {
                ESP_LOGE(TAG, "BLE validation stack fault; releasing BLE and returning to sleep without alert dispatch");
                reset_alert_context();
                s_ble_release_to_sleep = true;
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_RELEASING_BLE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_BLE_RELEASE_COMPLETE) {
                if (s_ble_release_to_sleep) {
                    s_ble_release_to_sleep = false;
                    orchestrator_enter_state(&state, STATE_SLEEP);
                } else if (s_alert_dispatch_pending) {
                    orchestrator_enter_state(&state, STATE_DISPATCHING_ALERT);
                } else {
                    s_ignition_webrtc_mode = s_pending_webrtc_mode;
                    orchestrator_enter_state(&state, STATE_IGNITING);
                }
            } else if (event == ORCH_EVENT_BLE_RELEASE_FAILED) {
                orchestrator_log_heap_snapshot("ble_release:failed_to_sleep");
                s_ble_release_to_sleep = false;
                orchestrator_enter_state(&state, STATE_SLEEP);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_DISPATCHING_ALERT:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_ALERT_DISPATCH_COMPLETE ||
                       event == ORCH_EVENT_ALERT_DISPATCH_FAILED)
            {
                if (!orchestrator_alert_result_matches(&event_msg)) {
                    ESP_LOGW(TAG, "Ignoring stale alert dispatch result");
                    break;
                }
                if (event == ORCH_EVENT_ALERT_DISPATCH_FAILED) {
                    ESP_LOGW(TAG, "Initial alert dispatch failed; continuing to Vigilante ignition");
                }
                s_ignition_webrtc_mode = s_pending_webrtc_mode;
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_IGNITING);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_IGNITING:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_WEBRTC_CONNECTED) {
                orchestrator_enter_state(&state, STATE_ACTIVE);
                if (app_startup_event_group != NULL) {
                    /* Event group bits updated by webrtc module directly */
                }
            } else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                       event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_ACTIVE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_MIC_MUTED || event == ORCH_EVENT_MIC_UNMUTED) {
                bool want_mute = (event == ORCH_EVENT_MIC_MUTED);
                if (s_is_muted != want_mute) {
                    s_is_muted = want_mute;
                }
                /* Actualizar timestamp de actividad por interacción física con el botón */
                webrtc_mark_activity();

                /* Synchronize UI Canvas State & Hardware Mic */
                if (event == ORCH_EVENT_MIC_MUTED) {
                    ui_show_status_message("Muted / Dozing", COLOR_RED_BGR565);
                    media_sys_mic_mute(true); /* Físicamente apagar el micrófono (Modo Normal) */
                    mute_handler_start_idle_timer();
                } else {
                    ui_clear_status_message();
                    media_sys_mic_mute(false); /* Reactivar micrófono físicamente (Modo Normal) */
                    mute_handler_stop_idle_timer();
                    webrtc_post_action(WEBRTC_ACTION_NOTIFY_UNMUTE);
                }
            } else if (event == ORCH_EVENT_IDLE_ALERT_START) {
                ui_clear_status_message();
                ui_show_status_message("You there?", COLOR_YELLOW_BGR565);
            } else if (event == ORCH_EVENT_IDLE_ALERT_END) {
                if (s_is_muted) {
                    ui_show_status_message("Muted / Dozing", COLOR_RED_BGR565);
                } else {
                    ui_clear_status_message();
                }
            } else if (event == ORCH_EVENT_AUTO_SLEEP_TIMEOUT &&
                       !orchestrator_is_vigilante_active())
            {
                s_teardown_ui_pending = true;
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            } else if (event == ORCH_EVENT_VIGILANTE_ROOM_VACATED ||
                       event == ORCH_EVENT_VIGILANTE_TIMEOUT)
            {
                ESP_LOGW(TAG, "Vigilante lifecycle ended by %s", orchestrator_event_name(event));
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            } else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                       event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            } else if (event == ORCH_EVENT_MOTION_DETECTED) {
                orchestrator_note_vigilante_motion_detected(&event_msg);
                orchestrator_ignore_event(state, event);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_AUTO_SLEEPING:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else {
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            }
            break;

        case STATE_STOPPING_WEBRTC:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED) {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            } else if (event == ORCH_EVENT_WEBRTC_STOPPED) {
                s_webrtc_stop_started_ms = 0;
                if (s_teardown_ui_pending) {
                    s_teardown_ui_pending = false;
                    orchestrator_enter_state(&state, STATE_TEARING_DOWN_UI_FOR_SCAN);
                } else {
                    orchestrator_enter_state(&state, STATE_SLEEP);
                }
            } else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                       event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                ESP_LOGI(TAG, "Ignoring WebRTC disconnect notification during local shutdown");
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_TEARING_DOWN_UI_FOR_SCAN:
            if (event == ORCH_EVENT_RAM_FREED) {
                ESP_LOGI(TAG, "STATE_TEARING_DOWN_UI_FOR_SCAN: UI is dead. System RAM freed for BLE scanning.");
                orchestrator_enter_state(&state, STATE_AUTO_ARM_BLE_SCANNING);
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_AUTO_ARM_BLE_SCANNING:
            if (event == ORCH_EVENT_BLE_READY) {
                ESP_LOGI(TAG, "STATE_AUTO_ARM_BLE_SCANNING: BLE stack ready. Starting headless identity validation...");
                orchestrator_start_identity_validation();
            } else if (event == ORCH_EVENT_BLE_BUSY) {
                ESP_LOGE(TAG, "STATE_AUTO_ARM_BLE_SCANNING: BLE stack failed to initialize. Arming system (CENTINELA).");
                nvs_set_operation_mode(BOOT_MODE_CENTINELA);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (event == ORCH_EVENT_IDENTITY_PRESENT) {
                ESP_LOGI(TAG, "STATE_AUTO_ARM_BLE_SCANNING: Identity FOUND. Re-routing boot to DIRECTO.");
                nvs_set_operation_mode(BOOT_MODE_DIRECTO);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (event == ORCH_EVENT_IDENTITY_REJECTED) {
                ESP_LOGW(TAG, "STATE_AUTO_ARM_BLE_SCANNING: Identity REJECTED. Arming system (CENTINELA).");
                nvs_set_operation_mode(BOOT_MODE_CENTINELA);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_FATAL_ERROR:
            if (event == ORCH_EVENT_FATAL_ERROR)
            {
                ESP_LOGE(TAG, "Executing Fatal Error Teardown...");

                /* 1. Update UI FIRST */
                /* Send text through the normal pipeline while Simi is ALIVE (triggers Delegation Trap) */
                ui_show_status_message("API Error 429", COLOR_RED_BGR565);

                /* Allow the Simi task time to render and flush the overlay frame to the LCD */
                vTaskDelay(pdMS_TO_TICKS(250));

                /* Kill the animation task, freezing the overlay frame permanently on the screen */

                /* 2. Forcefully stop hardware SECOND */
                // radar_hal_disable() removed
                csi_handler_stop();
                media_sys_teardown();

                /* 3. Safe Timeout */
                esp_timer_create_args_t timer_args = {
                    .callback = &fatal_error_timer_cb,
                    .name     = "fatal_reboot_timer"
                };
                esp_timer_handle_t reboot_timer;
                if (esp_timer_create(&timer_args, &reboot_timer) == ESP_OK) {
                    esp_timer_start_once(reboot_timer, 900000000ULL); /* 15 mins (microseconds) */
                }
            }
            else
            {
                /* Total Isolation: Ignore all other incoming events */
                orchestrator_ignore_event(state, event);
            }
            break;
        }
    }
}
