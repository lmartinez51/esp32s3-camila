/**
 * @file wifi_tcp.h
 * @brief Raw TCP socket driver for LAN robots (Phase 4).
 *
 * Endpoint format: "192.168.1.50:8000" (alias <-> ip:port).
 * Probe = bounded non-blocking TCP connect; command = payload bytes.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef WIFI_TCP_H
#define WIFI_TCP_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_TCP_PROFILE_ID "wifi_tcp"

/**
 * @brief Get the wifi_tcp driver descriptor (registered by device_registry).
 */
const robot_driver_t *wifi_tcp_get_driver(void);

/**
 * @brief Bounded non-blocking TCP reachability probe (shared transport helper).
 *
 * Used by the wifi_tcp and wifi_http drivers' probe(). Never blocks longer
 * than timeout_ms. A refused/blackholed peer or WiFi-down reports
 * present=false in ESP_OK (fail-fast offline answer).
 *
 * @param ip         IPv4 string ("192.168.1.50").
 * @param port       TCP port (1..65535).
 * @param timeout_ms Probe budget (use CONFIG_ROBOT_PROBE_TIMEOUT_MS).
 * @param present    Receives reachability (always written).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or a socket error code.
 */
esp_err_t wifi_tcp_probe_tcp(const char *ip, uint16_t port,
                             uint32_t timeout_ms, bool *present);

/**
 * @brief Resolve a robot_endpoint_t into ip/port for WiFi drivers.
 *
 * Prefers ep->ip/ep->port; falls back to parsing ep->endpoint ("ip:port").
 *
 * @param ep   Endpoint from the registry.
 * @param ip   Out: IPv4 string (buffer >= 16 bytes).
 * @param port Out: port.
 * @return true when both are valid.
 */
bool wifi_tcp_parse_endpoint(const robot_endpoint_t *ep, char ip[16],
                             uint16_t *port);

/**
 * @brief Send a payload string to an endpoint over a bounded TCP
 * connect+send+close (shared by wifi_arm / wifi_pantilt).
 *
 * @param ep      Device endpoint (ip/port fields or "ip:port" string).
 * @param payload NUL-terminated payload.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE (WiFi
 *         down) / ESP_ERR_NOT_FOUND (refused) / ESP_ERR_TIMEOUT / ESP_FAIL.
 */
esp_err_t wifi_tcp_send_endpoint(const robot_endpoint_t *ep,
                                 const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_TCP_H */
