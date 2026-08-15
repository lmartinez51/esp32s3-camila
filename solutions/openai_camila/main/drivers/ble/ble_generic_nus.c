/**
 * @file ble_generic_nus.c
 * @brief Generic NUS / serial BLE driver (Phase 3).
 *
 * execute() resolves the device through the HAL registry (endpoint MAC),
 * ensures a connection via the shared ble_transport phased-wait policy,
 * writes the payload mapped for the normalized action and, for timed
 * motion commands, schedules a generic pulse-stop. Telemetry actions are
 * NOT supported (ROBOT_RESULT_ERR_UNSUPPORTED → adapter legacy fallback).
 *
 * Payload table (documented default for HM-10/AT09 ASCII car kits):
 *   FORWARD "F" | BACKWARD "B" | LEFT "L" | RIGHT "R" | STOP "S"
 * Per-device payload overrides arrive with the Phase 4 registry
 * endpoint/payload configuration tools.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ble_generic_nus.h"

#include <string.h>
#include "esp_log.h"
#include "ble_transport.h"
#include "robot_hal.h"

#define TAG "BLE_GENERIC_NUS"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif

#define BLE_GENERIC_NUS_CAP_MASK ( \
    (1u << ROBOT_ACTION_FORWARD) | \
    (1u << ROBOT_ACTION_BACKWARD) | \
    (1u << ROBOT_ACTION_LEFT) | \
    (1u << ROBOT_ACTION_RIGHT) | \
    (1u << ROBOT_ACTION_STOP))

typedef struct
{
    robot_action_id_t action;
    const char *payload;
} ble_generic_payload_entry_t;

static const ble_generic_payload_entry_t s_generic_payloads[] = {
    { ROBOT_ACTION_FORWARD,  "F" },
    { ROBOT_ACTION_BACKWARD, "B" },
    { ROBOT_ACTION_LEFT,     "L" },
    { ROBOT_ACTION_RIGHT,    "R" },
    { ROBOT_ACTION_STOP,     "S" },
};

static esp_err_t ble_generic_init(robot_driver_t *drv)
{
    (void)drv;
    return ESP_OK;
}

static esp_err_t ble_generic_probe(robot_driver_t *drv,
                                   const robot_endpoint_t *ep,
                                   bool *present)
{
    (void)drv;
    if (!ep || !present)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool has_addr = false;
    for (int i = 0; i < 6; i++)
    {
        has_addr |= (ep->addr[i] != 0);
    }
    if (!has_addr)
    {
        *present = true; /* sin MAC no hay forma de sondear: asumir presente */
        return ESP_OK;
    }

    return ble_transport_probe_addr(ep->addr, ep->addr_type,
                                    CONFIG_ROBOT_PROBE_TIMEOUT_MS, present);
}

static robot_result_code_t ble_generic_map_error(esp_err_t err)
{
    switch (err)
    {
    case ESP_ERR_NOT_FOUND:      return ROBOT_RESULT_ERR_NOT_FOUND;
    case ESP_ERR_TIMEOUT:        return ROBOT_RESULT_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG:    return ROBOT_RESULT_ERR_INVALID_ARG;
    case ESP_ERR_NOT_SUPPORTED:  return ROBOT_RESULT_ERR_UNSUPPORTED;
    default:                     return ROBOT_RESULT_ERR_TRANSPORT;
    }
}

static const char *ble_generic_payload_for(robot_action_id_t action)
{
    for (size_t i = 0; i < sizeof(s_generic_payloads) / sizeof(s_generic_payloads[0]); i++)
    {
        if (s_generic_payloads[i].action == action)
        {
            return s_generic_payloads[i].payload;
        }
    }
    return NULL;
}

static esp_err_t ble_generic_execute(robot_driver_t *drv,
                                     const char *alias,
                                     robot_action_id_t action,
                                     const robot_action_params_t *params,
                                     robot_result_t *out)
{
    (void)drv;
    if (!alias || !out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->code = ROBOT_RESULT_ERR_TRANSPORT;

    if ((BLE_GENERIC_NUS_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), BLE_GENERIC_NUS_PROFILE_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *payload = ble_generic_payload_for(action);
    if (payload == NULL)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Sin payload configurado para '%s'",
                 robot_action_to_string(action));
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Resolver el endpoint del alias desde el registro HAL. */
    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        out->code = ROBOT_RESULT_ERR_NOT_FOUND;
        snprintf(out->detail, sizeof(out->detail), "Dispositivo '%s' no registrado", alias);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t conn_handle = 0;
    uint16_t char_handle = 0;
    esp_err_t err = ble_transport_connect_and_wait(dev->endpoint.addr, dev->endpoint.addr_type,
                                                   BLE_TRANSPORT_PHASE1_TIMEOUT_MS,
                                                   BLE_TRANSPORT_PHASE2_TIMEOUT_MS,
                                                   &conn_handle, &char_handle);
    if (err != ESP_OK)
    {
        out->code = ble_generic_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "BLE '%s' (%s): %s", alias, robot_action_to_string(action), esp_err_to_name(err));
        return err;
    }

    err = ble_transport_write_raw(conn_handle, char_handle,
                                  (const uint8_t *)payload, (uint16_t)strlen(payload));
    if (err != ESP_OK)
    {
        out->code = ble_generic_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "Escritura GATT fallida en '%s': %s", alias, esp_err_to_name(err));
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    strlcpy(out->detail, "Comando BLE ejecutado correctamente", sizeof(out->detail));

    uint32_t duration_ms = params ? params->duration_ms : 0;
    if (action != ROBOT_ACTION_STOP && duration_ms > 0)
    {
        ble_transport_pulse_stop(conn_handle, char_handle, duration_ms,
                                 (const uint8_t *)"S", 1);
    }

    return ESP_OK;
}

static const robot_driver_t s_ble_generic_nus_driver = {
    .profile_id = BLE_GENERIC_NUS_PROFILE_ID,
    .category   = ROBOT_CATEGORY_GENERIC,
    .protocol   = ROBOT_PROTOCOL_BLE,
    .capabilities = BLE_GENERIC_NUS_CAP_MASK,
    .init       = ble_generic_init,
    .execute    = ble_generic_execute,
    .probe      = ble_generic_probe,
    .deinit     = NULL,
    .priv       = NULL,
};

const robot_driver_t *ble_generic_nus_get_driver(void)
{
    return &s_ble_generic_nus_driver;
}
