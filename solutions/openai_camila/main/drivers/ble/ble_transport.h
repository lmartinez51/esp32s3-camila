/**
 * @file ble_transport.h
 * @brief Shared BLE central transport — probe, connect policy, raw I/O (Phase 3).
 *
 * This module is the first carved slice of the legacy transport policy
 * (architecture doc, coupling item C8). It provides:
 *
 *  - bounded PASSIVE scan probes (never connects) → <500 ms offline answers;
 *  - the phased-wait connect policy (8 s link / 6 s GATT, abort on early
 *    failure) — the same policy the legacy on-demand path uses, implemented
 *    here as a reader over the legacy device table so both can coexist;
 *  - raw GATT writes and a generic pulse-stop helper.
 *
 * The legacy ble_device_control module remains the connection owner
 * (NimBLE role + GATT discovery); this transport never initiates a connect
 * that legacy did not start, and never mutates legacy state.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_TRANSPORT_H
#define BLE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Phased-wait budgets & retry policies (mirror on-demand policy) */
#define BLE_TRANSPORT_MAX_RETRIES        3u
#define BLE_TRANSPORT_RETRY_BACKOFF_MS   120u
#define BLE_TRANSPORT_PHASE1_TIMEOUT_MS  1500u  /* link establishment    */
#define BLE_TRANSPORT_PHASE2_TIMEOUT_MS  6000u  /* GATT discovery        */

/**
 * @brief Bounded passive scan probe — MAC match.
 *
 * Scans passively for up to `timeout_ms`, stopping as soon as the MAC is
 * seen (or when the window elapses). NEVER connects and NEVER mutates the
 * legacy device table. Called from the adapter task (bounded exception to
 * the non-blocking rule, per architecture doc §3.4).
 *
 * @param addr      BLE MAC to look for (6 bytes).
 * @param addr_type BLE address type.
 * @param timeout_ms Scan budget (e.g. CONFIG_ROBOT_PROBE_TIMEOUT_MS).
 * @param present   Out: true if seen, false otherwise.
 * @return ESP_OK (probe ran), ESP_ERR_INVALID_STATE (NimBLE not synced),
 *         ESP_ERR_NO_MEM. If a scan is already active the probe degrades
 *         gracefully: present=true, ESP_OK.
 */
esp_err_t ble_transport_probe_addr(const uint8_t addr[6], uint8_t addr_type,
                                   uint32_t timeout_ms, bool *present);

/**
 * @brief Bounded passive scan probe — name substring match.
 */
esp_err_t ble_transport_probe_name(const char *name, uint32_t timeout_ms, bool *present);

/**
 * @brief Connect-or-reuse with phased wait (reader over legacy state).
 *
 * If the device is already connected and GATT-discovered, returns the
 * cached handles immediately. Otherwise asks the legacy module to connect
 * (ble_device_connect) and waits with the phased policy: link ≤ phase1,
 * GATT discovery ≤ phase2 (measured from link), aborting as soon as the
 * device leaves CONNECTING without a link.
 *
 * @param addr      Target MAC.
 * @param addr_type Address type.
 * @param phase1_timeout_ms Link budget (use BLE_TRANSPORT_PHASE1_TIMEOUT_MS).
 * @param phase2_timeout_ms GATT budget (use BLE_TRANSPORT_PHASE2_TIMEOUT_MS).
 * @param out_conn_handle   Out: live connection handle (0 on failure).
 * @param out_char_handle   Out: GATT value handle for writes.
 * @return ESP_OK, ESP_ERR_NOT_FOUND (not in legacy table), or ESP_FAIL.
 */
esp_err_t ble_transport_connect_and_wait(const uint8_t addr[6], uint8_t addr_type,
                                         uint32_t phase1_timeout_ms,
                                         uint32_t phase2_timeout_ms,
                                         uint16_t *out_conn_handle,
                                         uint16_t *out_char_handle);

/**
 * @brief Raw GATT write (Write Without Response, fire-and-forget).
 */
esp_err_t ble_transport_write_raw(uint16_t conn_handle, uint16_t char_handle,
                                  const uint8_t *data, uint16_t len);

/**
 * @brief Generic pulse-stop: send `stop_data` after `delay_ms`, then terminate.
 *
 * Clones the legacy ELEGOO pulse-stop task pattern but takes a caller-owned
 * payload (NUS/serial drivers send their own STOP bytes). The spawned task
 * runs on a PSRAM stack (fallback internal) and owns the data copy.
 *
 * @param conn_handle Live connection (must remain valid until the task runs).
 * @param char_handle Write handle.
 * @param delay_ms    Pulse duration before STOP.
 * @param stop_data   STOP payload bytes (copied).
 * @param stop_len    STOP payload length.
 */
esp_err_t ble_transport_pulse_stop(uint16_t conn_handle, uint16_t char_handle,
                                   uint32_t delay_ms,
                                   const uint8_t *stop_data, uint16_t stop_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TRANSPORT_H */
