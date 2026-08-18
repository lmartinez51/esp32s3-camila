/**
 * @file audio_watchdog.h
 * @brief Watchdog de diagnóstico del pipeline de audio (AFE fetch). NO intrusivo.
 */

#ifndef AUDIO_WATCHDOG_H
#define AUDIO_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia el watchdog de diagnóstico del audio (idempotente).
 *
 * Tarea de baja prioridad (2) en core 1 que detecta stall de fetch() del AFE
 * y reporta estados de tareas + heap. No toca el pipeline de audio.
 */
void audio_watchdog_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_WATCHDOG_H */