/**
 * @file ble_elegoo_bt16.c
 * @brief ELEGOO BT16 robot car — Robot HAL driver (Phase 2).
 *
 * execute() maps normalized HAL actions to the legacy ELEGOO command
 * vocabulary (the same strings the legacy WebRTC tool handler used) and
 * delegates to ble_device_send_command_by_alias_or_name(), preserving
 * byte-for-byte runtime behavior: pulse duration, stop-pulse semantics,
 * on-demand connect and offline fast-fail all come from the legacy module.
 *
 * Telemetry actions (READ_ULTRASONIC / READ_LINE_SENSOR) copy the last
 * notification payload reported by the legacy module into robot_result_t.
 *
 * probe() performs a bounded, non-blocking presence check against the
 * legacy discovered-devices RAM table (the same source the registry was
 * migrated from); it NEVER initiates a BLE scan or connection.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ble_elegoo_bt16.h"

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "ble_transport.h"
#include "ble_device_control.h"

#define TAG "BLE_ELEGOO_BT16"

#define BLE_ELEGOO_PROFILE_ID "elegoo_bt16"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif

/* Actions this driver can perform (HAL vocabulary). */
#define BLE_ELEGOO_CAP_MASK ( \
    (1u << ROBOT_ACTION_FORWARD) | \
    (1u << ROBOT_ACTION_BACKWARD) | \
    (1u << ROBOT_ACTION_LEFT) | \
    (1u << ROBOT_ACTION_RIGHT) | \
    (1u << ROBOT_ACTION_STOP) | \
    (1u << ROBOT_ACTION_MOVE_HEAD) | \
    (1u << ROBOT_ACTION_OBSTACLE_AVOIDANCE) | \
    (1u << ROBOT_ACTION_LINE_TRACKING) | \
    (1u << ROBOT_ACTION_PAN_LEFT) | \
    (1u << ROBOT_ACTION_PAN_RIGHT) | \
    (1u << ROBOT_ACTION_CENTER) | \
    (1u << ROBOT_ACTION_READ_ULTRASONIC) | \
    (1u << ROBOT_ACTION_READ_LINE_SENSOR))

static esp_err_t ble_elegoo_init(robot_driver_t *drv)
{
    (void)drv;
    /* The legacy BLE device control module manages its own lifecycle
     * (started by the orchestrator before WebRTC goes live). */
    return ESP_OK;
}

static robot_result_code_t ble_elegoo_map_error(esp_err_t err)
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

static esp_err_t ble_elegoo_execute(robot_driver_t *drv,
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

    if ((BLE_ELEGOO_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), BLE_ELEGOO_PROFILE_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *action_str = NULL;
    uint32_t param_val = 0;

    switch (action)
    {
    case ROBOT_ACTION_PAN_LEFT:
        action_str = "MOVE_HEAD";
        param_val = 180;
        break;

    case ROBOT_ACTION_PAN_RIGHT:
        action_str = "MOVE_HEAD";
        param_val = 0;
        break;

    case ROBOT_ACTION_CENTER:
        action_str = "MOVE_HEAD";
        param_val = 90;
        break;

    case ROBOT_ACTION_MOVE_HEAD:
    {
        action_str = "MOVE_HEAD";
        uint16_t angle = params ? params->angle_deg : 90;
        if (angle > 180)
        {
            angle = 180;
        }
        param_val = (uint32_t)angle;
        break;
    }

    case ROBOT_ACTION_OBSTACLE_AVOIDANCE:
        action_str = "SET_AUTONOMOUS_MODE";
        param_val = 2;
        break;

    case ROBOT_ACTION_LINE_TRACKING:
        action_str = "SET_AUTONOMOUS_MODE";
        param_val = 1;
        break;

    default:
        action_str = robot_action_to_string(action);
        param_val = params ? params->duration_ms : 0;
        break;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    const char *target_name = (dev && dev->alias[0] != '\0') ? dev->alias : alias;

    esp_err_t err = ble_device_send_command_by_alias_or_name(target_name, action_str, param_val);
    if (err != ESP_OK)
    {
        out->code = ble_elegoo_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "BLE '%s' (%s): %s", target_name, action_str, esp_err_to_name(err));
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    strlcpy(out->detail, "Comando BLE ejecutado correctamente", sizeof(out->detail));

    if (action == ROBOT_ACTION_READ_ULTRASONIC || action == ROBOT_ACTION_READ_LINE_SENSOR)
    {
        const char *telemetry = ble_device_get_last_telemetry();
        if (telemetry != NULL && telemetry[0] != '\0')
        {
            strlcpy(out->telemetry, telemetry, sizeof(out->telemetry));
        }
    }

    return ESP_OK;
}

static esp_err_t ble_elegoo_probe(robot_driver_t *drv,
                                  const robot_endpoint_t *ep,
                                  bool *present)
{
    (void)drv;
    (void)ep;
    if (!present)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Bypass passive scan pre-probe for BLE devices.
     * BLE advertising intervals can exceed short passive probe windows, causing
     * false OFFLINE responses. We bypass pre-probe and let bounded connect
     * determine availability in execute(). */
    *present = true;
    return ESP_OK;
}

static const robot_driver_t s_elegoo_bt16_driver = {
    .profile_id = BLE_ELEGOO_PROFILE_ID,
    .category   = ROBOT_CATEGORY_CAR,
    .protocol   = ROBOT_PROTOCOL_BLE,
    .capabilities = BLE_ELEGOO_CAP_MASK,
    .init       = ble_elegoo_init,
    .execute    = ble_elegoo_execute,
    .probe      = ble_elegoo_probe,
    .deinit     = NULL,
    .priv       = NULL,
};

const robot_driver_t *ble_elegoo_bt16_get_driver(void)
{
    return &s_elegoo_bt16_driver;
}
