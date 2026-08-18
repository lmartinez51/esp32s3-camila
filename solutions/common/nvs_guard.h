/**
 * @file nvs_guard.h
 * @brief Global recursive mutex that serializes ALL NVS access across the project.
 *
 * Lives in the shared `common` component so every NVS consumer — solution code
 * (config/nvs_setup.c, registry_persist.c, ir_rmt.c, ble_device_control.c) and
 * common code (network_storage.c) — synchronizes on the SAME mutex.
 *
 * Usage contract:
 *   - Call nvs_setup_mutex_init() once before first use (idempotent).
 *   - Wrap every nvs_open()...nvs_close() section with nvs_lock()/nvs_unlock().
 *   - The mutex is recursive: nested lock/unlock from the same task is safe.
 *
 * @date 2026
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Creates the global recursive NVS mutex if it does not exist yet.
     * Idempotent and safe to call from any task, any number of times.
     */
    void nvs_setup_mutex_init(void);

    /**
     * @brief Acquires the global NVS mutex (blocking, portMAX_DELAY).
     * No-op if the mutex has not been initialized.
     */
    void nvs_lock(void);

    /**
     * @brief Releases the global NVS mutex.
     * No-op if the mutex has not been initialized.
     */
    void nvs_unlock(void);

#ifdef __cplusplus
}
#endif