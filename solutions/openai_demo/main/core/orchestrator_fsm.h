/**
 * @file orchestrator_fsm.h
 * @brief Orchestrator finite-state machine: state transitions and main task.
 *
 * Exposes the two functions that constitute the state machine:
 *   - orchestrator_enter_state(): performs all entry actions for a state.
 *   - app_startup_orchestrator_task(): the FreeRTOS task body; registers the
 *     task with xTaskCreate() from app_main.
 */
#pragma once

#include "orchestrator_helpers.h"

/**
 * @brief Transition the orchestrator to @p next_state and run entry actions.
 *
 * Idempotent: if *state == next_state the function logs and returns without
 * re-executing entry actions.
 *
 * @param state      Pointer to the current state variable (updated in-place).
 * @param next_state The target state to enter.
 */
void orchestrator_enter_state(orchestrator_state_t *state,
                              orchestrator_state_t  next_state);

/**
 * @brief FreeRTOS task body for the orchestrator state machine.
 *
 * Blocks on the orchestrator event queue and drives state transitions.
 * Must be created with xTaskCreate() from app_main.
 *
 * @param param Unused; pass NULL.
 */
void app_startup_orchestrator_task(void *param);
