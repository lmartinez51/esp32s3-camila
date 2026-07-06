/**
 * @file orchestrator_tasks.h
 * @brief Asynchronous FreeRTOS task launchers for the orchestrator.
 *
 * Declares the functions that create the short-lived tasks responsible for
 * BLE lifecycle management, alert dispatching, WebRTC shutdown, and the
 * CSI cooldown delay before motion sensing begins.
 *
 * All underlying task functions are static within orchestrator_tasks.c;
 * only the "start" entry points are exposed here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Cold-boot the BLE host and prepare the GAP scanner for identity
 *        validation.  Posts ORCH_EVENT_BLE_READY on success or
 *        ORCH_EVENT_BLE_BUSY on any failure.
 */
void orchestrator_start_ble_prepare(void);

/**
 * @brief Start bounded BLE identity validation after BLE has been prepared.
 *
 * Posts ORCH_EVENT_BLE_BUSY if the validation could not be scheduled.
 */
void orchestrator_start_identity_validation(void);

/**
 * @brief Release all BLE resources asynchronously.
 *
 * Posts ORCH_EVENT_BLE_RELEASE_COMPLETE or ORCH_EVENT_BLE_RELEASE_FAILED.
 */
void orchestrator_start_ble_release(void);

/**
 * @brief Dispatch an alert via alert_dispatcher_send_alert() in a dedicated task.
 *
 * @param timestamp_ms  Timestamp of the triggering motion event.
 * @param corr_drop     Correlation drop value from the sensing subsystem.
 * @param reinforcement If true, this is a reinforcement alert (does not post
 *                      result event to the orchestrator queue).
 * @return ESP_OK if the task was created, ESP_ERR_INVALID_STATE if one is
 *         already running, ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t orchestrator_start_alert_dispatch(uint32_t timestamp_ms,
                                            float    corr_drop,
                                            bool     reinforcement);

/**
 * @brief Stop the WebRTC stack asynchronously.
 *
 * Posts ORCH_EVENT_WEBRTC_STOPPED when done.  Safe to call even if the stack
 * is already stopped (the event will still be posted).
 */
void orchestrator_start_webrtc_stop(void);

/**
 * @brief Returns true if the WebRTC stop task has been running for longer
 *        than WEBRTC_STOP_TIMEOUT_MS.
 */
bool orchestrator_webrtc_stop_timeout_expired(void);

/**
 * @brief Schedule the post-sleep CSI cooldown delay before motion sensing
 *        is enabled.
 *
 * Creates a short-lived task that waits SLEEP_CSI_COOLDOWN_MS, shows the
 * "Warming up" banner, and then starts the CSI or hardware radar handler.
 * A generation counter is used to cancel stale tasks automatically.
 */
void orchestrator_schedule_sleep_csi_start(void);
