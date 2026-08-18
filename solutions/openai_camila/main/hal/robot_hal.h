/**
 * @file robot_hal.h
 * @brief Robot HAL public facade — protocol-agnostic device control.
 *
 * The HAL owns:
 *  - the normalized action vocabulary (robot_action_id_t),
 *  - the driver table (robot_driver_t vtable),
 *  - the device registry (alias <-> endpoint <-> driver profile),
 *  - the execute entry point used by the WebRTC tool adapter.
 *
 * Multi-protocol architecture (Phases 2–7): BLE (elegoo_bt16,
 * ble_generic_nus), WiFi (wifi_tcp, wifi_http, wifi_arm, wifi_pantilt)
 * and IR (ir_nec, ir_sony, ir_rc5) drivers, a persistent registry
 * (NVS blobs + legacy migration), and Phase 7 hardening: per-driver
 * in-flight cap, execute watchdog and the health monitor
 * (robot_health).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef ROBOT_HAL_H
#define ROBOT_HAL_H

#include "esp_err.h"
#include "robot_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Driver vtable ───────────────────────────────────────────────────── */

typedef struct robot_driver robot_driver_t;

typedef esp_err_t (*robot_driver_init_fn)(robot_driver_t *drv);
typedef esp_err_t (*robot_driver_execute_fn)(robot_driver_t *drv,
                                             const char *alias,
                                             robot_action_id_t action,
                                             const robot_action_params_t *params,
                                             robot_result_t *out);
/* Fail-fast connectivity probe: MUST return in < ROBOT_PROBE_TIMEOUT_MS (500). */
typedef esp_err_t (*robot_driver_probe_fn)(robot_driver_t *drv,
                                           const robot_endpoint_t *ep,
                                           bool *present);
typedef void (*robot_driver_deinit_fn)(robot_driver_t *drv);

typedef struct robot_driver
{
    char               profile_id[ROBOT_PROFILE_ID_MAX_LEN]; /* "elegoo_bt16" ... */
    robot_category_t   category;
    robot_protocol_t   protocol;
    uint32_t           capabilities; /* bitmask of robot_action_id_t supported */
    robot_driver_init_fn    init;
    robot_driver_execute_fn execute;
    robot_driver_probe_fn   probe;
    robot_driver_deinit_fn  deinit;
    void *priv;                    /* driver instance context (PSRAM) */
} robot_driver_t;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Initialize the Robot HAL (registry array in PSRAM + mutex).
 * @return ESP_OK on success (idempotent).
 */
esp_err_t robot_hal_init(void);

/* ── Driver table ────────────────────────────────────────────────────── */

/**
 * @brief Register a concrete driver (e.g. ELEGOO BT16 in Phase 2).
 * @param drv Driver descriptor (must be statically allocated).
 */
esp_err_t robot_hal_register_driver(const robot_driver_t *drv);

/* ── Device registry ─────────────────────────────────────────────────── */

/**
 * @brief Register (or update) a device in the RAM registry.
 * @param dev Device descriptor (copied into the PSRAM registry).
 */
esp_err_t robot_hal_register_device(const robot_device_t *dev);

/**
 * @brief Look up a device by exact alias match.
 * @return Pointer into the registry (valid until next mutation) or NULL.
 */
const robot_device_t *robot_hal_get_device(const char *alias);

/**
 * @brief Remove a device from the RAM registry (exact or fuzzy alias/endpoint
 * match). The persisted copy is NOT touched — callers must queue the NVS
 * removal themselves (registry_delete_device_async / ir_learn_delete).
 * @return ESP_OK when removed, ESP_ERR_NOT_FOUND when not registered.
 */
esp_err_t robot_hal_unregister_device(const char *alias);

/**
 * @brief Rename a registered device in the RAM registry (exact alias or
 * endpoint match). The persisted registry NVS blob keeps the old alias —
 * callers re-queue the device with registry_save_device_async() if needed.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when not registered.
 */
esp_err_t robot_hal_set_device_alias(const char *current_alias, const char *new_alias);

/**
 * @brief Remove every device from the RAM registry.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before robot_hal_init().
 */
esp_err_t robot_hal_clear_devices(void);

/**
 * @brief Get a registered device by index (Phase 7: health/integrity check).
 * @param idx Index in [0, robot_hal_get_device_count()).
 * @return Pointer to the device snapshot or NULL (out of range / not inited).
 */
const robot_device_t *robot_hal_get_device_at(size_t idx);

/**
 * @brief Get the number of registered devices.
 */
size_t robot_hal_get_device_count(void);

/**
 * @brief Number of registered drivers (Phase 6: dynamic tool catalog).
 */
size_t robot_hal_get_driver_count(void);

/**
 * @brief Get a registered driver by index (Phase 6: dynamic tool catalog).
 * @param idx Index in [0, robot_hal_get_driver_count()).
 * @return Driver descriptor pointer or NULL.
 */
const robot_driver_t *robot_hal_get_driver_at(size_t idx);

/**
 * @brief In-flight operation counter of a driver (Phase 7: health monitor).
 * @param idx Index in [0, robot_hal_get_driver_count()).
 * @return Number of operations currently executing on the driver.
 */
uint32_t robot_hal_get_driver_inflight_at(size_t idx);

/**
 * @brief Update the runtime presence cache of a registered device.
 *
 * present=true stores last_seen_ms = now (probe cache TTL); present=false
 * clears the cache (last_seen_ms = 0) so the device is re-probed on the
 * next command — offline answers are never cached.
 *
 * @param alias   Device alias.
 * @param present New presence state.
 * @return ESP_OK, or ESP_ERR_NOT_FOUND when the alias is not registered.
 */
esp_err_t robot_hal_set_device_presence(const char *alias, bool present);

/* ── Command execution ───────────────────────────────────────────────── */

/**
 * @brief Execute a normalized action on the device matching `alias`.
 *
 * Non-blocking contract: this function resolves the device/driver and
 * returns; any transport I/O is enqueued on the driver's own worker task.
 * The only bounded synchronous operation allowed is the fail-fast probe.
 *
 * @param alias  Device alias ("Carro").
 * @param action Normalized action ID.
 * @param params Action parameters (may be NULL when unused).
 * @param out    Result buffer (always written).
 * @return ESP_OK + ROBOT_RESULT_OK on success;
 *         ESP_OK + ROBOT_RESULT_ERR_OFFLINE when the probe says the device
 *         is unreachable (fail-fast, ≤ ROBOT_PROBE_TIMEOUT_MS; the caller
 *         must NOT fall back to another transport);
 *         ESP_ERR_NOT_FOUND when the alias is not in the registry;
 *         ESP_ERR_NOT_SUPPORTED when no driver handles it;
 *         other driver-specific errors otherwise.
 *
 * @note Fail-fast probe (Phase 3): before executing, the driver's probe()
 *       runs unless the presence cache is fresh (last probe or successful
 *       command within CONFIG_ROBOT_PROBE_CACHE_MS). Probe is the only
 *       bounded synchronous call allowed in this path (< 500 ms budget).
 */
esp_err_t robot_hal_execute(const char *alias,
                            robot_action_id_t action,
                            const robot_action_params_t *params,
                            robot_result_t *out);

/* ── Action vocabulary helpers ───────────────────────────────────────── */

robot_action_id_t robot_action_from_string(const char *s);
const char *robot_action_to_string(robot_action_id_t action);

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_HAL_H */
