/**
 * @file wifi_arm.c
 * @brief Robotic-arm driver over LAN TCP (Phase 6) implementation.
 *
 * Reuses the bounded TCP transport helpers of wifi_tcp (probe + send):
 * the probe is a non-blocking connect polled with select() up to
 * CONFIG_ROBOT_PROBE_TIMEOUT_MS and commands ride wifi_tcp_send_endpoint
 * with the CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS budget.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "wifi_arm.h"

#include <string.h>
#include "esp_log.h"
#include "wifi_tcp.h"
#include "robot_hal.h"

#define TAG "WIFI_ARM"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif

#define WIFI_ARM_CAP_MASK ( \
    (1u << ROBOT_ACTION_GRAB) | \
    (1u << ROBOT_ACTION_RELEASE) | \
    (1u << ROBOT_ACTION_ARM_UP) | \
    (1u << ROBOT_ACTION_ARM_DOWN) | \
    (1u << ROBOT_ACTION_ARM_HOME) | \
    (1u << ROBOT_ACTION_MOVE_AXIS))

typedef struct
{
    robot_action_id_t action;
    const char *payload;
} wifi_arm_payload_entry_t;

static const wifi_arm_payload_entry_t s_arm_payloads[] = {
    { ROBOT_ACTION_GRAB,     "GRAB" },
    { ROBOT_ACTION_RELEASE,  "RELEASE" },
    { ROBOT_ACTION_ARM_UP,   "UP" },
    { ROBOT_ACTION_ARM_DOWN, "DOWN" },
    { ROBOT_ACTION_ARM_HOME, "HOME" },
};

static robot_result_code_t wifi_arm_map_error(esp_err_t err)
{
    switch (err)
    {
    case ESP_ERR_TIMEOUT:        return ROBOT_RESULT_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG:    return ROBOT_RESULT_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE:  return ROBOT_RESULT_ERR_OFFLINE;
    default:                     return ROBOT_RESULT_ERR_TRANSPORT;
    }
}

static esp_err_t wifi_arm_probe(robot_driver_t *drv,
                                const robot_endpoint_t *ep,
                                bool *present)
{
    (void)drv;
    if (ep == NULL || present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *present = false;

    char ip[16];
    uint16_t port = 0;
    if (!wifi_tcp_parse_endpoint(ep, ip, &port))
    {
        ESP_LOGW(TAG, "Endpoint invalido, reportando no presente");
        return ESP_OK;
    }

    return wifi_tcp_probe_tcp(ip, port, CONFIG_ROBOT_PROBE_TIMEOUT_MS, present);
}

static esp_err_t wifi_arm_execute(robot_driver_t *drv,
                                  const char *alias,
                                  robot_action_id_t action,
                                  const robot_action_params_t *params,
                                  robot_result_t *out)
{
    (void)drv;
    if (alias == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->code = ROBOT_RESULT_ERR_TRANSPORT;

    if ((WIFI_ARM_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), WIFI_ARM_PROFILE_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    char payload[32];
    const char *cmd = NULL;
    if (action == ROBOT_ACTION_MOVE_AXIS)
    {
        const uint8_t axis = params ? params->axis_id : 0;
        const int32_t angle = params ? params->angle_deg : 0;
        snprintf(payload, sizeof(payload), "AXIS:%u:%d", (unsigned)axis, (int)angle);
        cmd = payload;
    }
    else
    {
        for (size_t i = 0; i < sizeof(s_arm_payloads) / sizeof(s_arm_payloads[0]); i++)
        {
            if (s_arm_payloads[i].action == action)
            {
                cmd = s_arm_payloads[i].payload;
                break;
            }
        }
        if (cmd == NULL)
        {
            out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
            snprintf(out->detail, sizeof(out->detail),
                     "Sin payload configurado para '%s'", robot_action_to_string(action));
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        out->code = ROBOT_RESULT_ERR_NOT_FOUND;
        snprintf(out->detail, sizeof(out->detail), "Dispositivo '%s' no registrado", alias);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t err = wifi_tcp_send_endpoint(&dev->endpoint, cmd);
    if (err != ESP_OK)
    {
        out->code = wifi_arm_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "Brazo '%s': %s", alias, esp_err_to_name(err));
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    snprintf(out->detail, sizeof(out->detail),
             "Comando de brazo '%s' ejecutado en '%s'",
             robot_action_to_string(action), alias);
    return ESP_OK;
}

static const robot_driver_t s_wifi_arm_driver = {
    .profile_id = WIFI_ARM_PROFILE_ID,
    .category   = ROBOT_CATEGORY_ARM,
    .protocol   = ROBOT_PROTOCOL_WIFI,
    .capabilities = WIFI_ARM_CAP_MASK,
    .init       = NULL,
    .execute    = wifi_arm_execute,
    .probe      = wifi_arm_probe,
    .deinit     = NULL,
    .priv       = NULL,
};

const robot_driver_t *wifi_arm_get_driver(void)
{
    return &s_wifi_arm_driver;
}
