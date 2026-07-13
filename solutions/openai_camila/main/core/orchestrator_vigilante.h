/**
 * @file orchestrator_vigilante.h
 * @brief Vigilante-mode presence monitor for the orchestrator state machine.
 *
 * Provides two functions called exclusively from STATE_ACTIVE:
 *   - orchestrator_note_vigilante_motion_detected(): resets the resting window
 *     when a new motion event arrives.
 *   - orchestrator_poll_vigilante_monitor(): periodic poll that synthesises
 *     the ROOM_VACATED and TIMEOUT synthetic events, and schedules the
 *     reinforcement alert when the threshold is reached.
 */
#pragma once

#include <stdbool.h>
#include "app_events.h"

/**
 * @brief Notify the Vigilante monitor that a new motion event was received.
 *
 * No-op if Vigilante mode is not active. Resets the resting window so the
 * room-vacated timer restarts from zero.
 *
 * @param msg Pointer to the triggering event message (for logging).
 */
void orchestrator_note_vigilante_motion_detected(const orchestrator_event_msg_t *msg);

/**
 * @brief Poll the Vigilante monitor and synthesise timeout/vacated events.
 *
 * Must be called periodically (every AUTO_SLEEP_POLL_MS) from the
 * orchestrator task when state == STATE_ACTIVE and Vigilante mode is active.
 *
 * @param out_msg Output event message to post to the orchestrator queue.
 * @return true if @p out_msg was filled with a synthetic event, false otherwise.
 */
bool orchestrator_poll_vigilante_monitor(orchestrator_event_msg_t *out_msg);
