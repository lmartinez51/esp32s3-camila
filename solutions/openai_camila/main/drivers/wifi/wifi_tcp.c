/**
 * @file wifi_tcp.c
 * @brief Raw TCP socket driver for LAN robots (Phase 4).
 *
 * Payload table (v1 default, ASCII car kits over TCP):
 *   FORWARD "F" | BACKWARD "B" | LEFT "L" | RIGHT "R" | STOP "S"
 * Per-device payload overrides arrive with later registry config tools.
 *
 * All socket I/O is bounded: the probe is a non-blocking connect polled
 * with select() up to CONFIG_ROBOT_PROBE_TIMEOUT_MS, and the command path
 * uses a connect+send with the same technique up to
 * CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS — the voice round-trip stays bounded.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "wifi_tcp.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_session_state.h"
#include "robot_hal.h"

#define TAG "WIFI_TCP"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif
#if !defined(CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS)
#define CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS 1500
#endif

#define WIFI_TCP_STOP_DATA_MAX 16u
#define WIFI_TCP_PULSE_TASK_STACK 3072u

#define WIFI_TCP_CAP_MASK ( \
    (1u << ROBOT_ACTION_FORWARD) | \
    (1u << ROBOT_ACTION_BACKWARD) | \
    (1u << ROBOT_ACTION_LEFT) | \
    (1u << ROBOT_ACTION_RIGHT) | \
    (1u << ROBOT_ACTION_STOP))

typedef struct
{
    robot_action_id_t action;
    const char *payload;
} wifi_tcp_payload_entry_t;

static const wifi_tcp_payload_entry_t s_tcp_payloads[] = {
    { ROBOT_ACTION_FORWARD,  "F" },
    { ROBOT_ACTION_BACKWARD, "B" },
    { ROBOT_ACTION_LEFT,     "L" },
    { ROBOT_ACTION_RIGHT,    "R" },
    { ROBOT_ACTION_STOP,     "S" },
};

static bool wifi_tcp_network_up(void)
{
    const char *ssid = wifi_session_get_connected_ssid();
    return (ssid != NULL && ssid[0] != '\0');
}

bool wifi_tcp_parse_endpoint(const robot_endpoint_t *ep, char ip[16], uint16_t *port)
{
    if (ep == NULL || ip == NULL || port == NULL)
    {
        return false;
    }

    if (ep->ip[0] != '\0' && ep->port != 0)
    {
        strlcpy(ip, ep->ip, 16);
        *port = ep->port;
        return true;
    }

    /* Fallback: parse "ip:port" from the serialized endpoint string. */
    if (ep->endpoint[0] != '\0')
    {
        const char *colon = strrchr(ep->endpoint, ':');
        if (colon == NULL)
        {
            return false;
        }
        const size_t ip_len = (size_t)(colon - ep->endpoint);
        if (ip_len == 0 || ip_len >= 16)
        {
            return false;
        }
        memcpy(ip, ep->endpoint, ip_len);
        ip[ip_len] = '\0';
        char *end = NULL;
        const long p = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || p <= 0 || p > 65535)
        {
            return false;
        }
        *port = (uint16_t)p;
        return true;
    }

    return false;
}

/* Non-blocking connect polled with select(); returns a connected socket. */
static esp_err_t wifi_tcp_connect_bounded(const char *ip, uint16_t port,
                                          uint32_t timeout_ms, int *sock_out)
{
    *sock_out = -1;

    if (!wifi_tcp_network_up())
    {
        ESP_LOGD(TAG, "WiFi no conectado: '%s' no alcanzable", ip);
        return ESP_ERR_INVALID_STATE;
    }

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket() fallo: errno=%d", errno);
        return ESP_FAIL;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
    {
        ESP_LOGW(TAG, "IP invalida: '%s'", ip);
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0)
    {
        if (errno == EINPROGRESS || errno == EWOULDBLOCK)
        {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            struct timeval tv = {
                .tv_sec  = (timeout_ms / 1000),
                .tv_usec = (timeout_ms % 1000) * 1000,
            };
            rc = select(sock + 1, NULL, &wfds, NULL, &tv);
            if (rc > 0)
            {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &slen) != 0 || so_err != 0)
                {
                    ESP_LOGD(TAG, "Conexion a %s:%u rechazada (SO_ERROR=%d)", ip, port, so_err);
                    close(sock);
                    return ESP_ERR_NOT_FOUND;
                }
            }
            else
            {
                ESP_LOGD(TAG, "Conexion a %s:%u sin respuesta en %lu ms", ip, port,
                         (unsigned long)timeout_ms);
                close(sock);
                return ESP_ERR_TIMEOUT;
            }
        }
        else if (errno == EISCONN)
        {
            /* ya conectada (caso raro en localhost) */
        }
        else
        {
            ESP_LOGD(TAG, "connect(%s:%u) fallo: errno=%d", ip, port, errno);
            close(sock);
            return ESP_ERR_NOT_FOUND;
        }
    }

    *sock_out = sock;
    return ESP_OK;
}

esp_err_t wifi_tcp_probe_tcp(const char *ip, uint16_t port,
                             uint32_t timeout_ms, bool *present)
{
    if (ip == NULL || ip[0] == '\0' || port == 0 || present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *present = false;

    int sock = -1;
    esp_err_t err = wifi_tcp_connect_bounded(ip, port, timeout_ms, &sock);
    if (err == ESP_OK)
    {
        *present = true;
        close(sock);
    }
    else if (err != ESP_ERR_INVALID_STATE &&
             err != ESP_ERR_TIMEOUT &&
             err != ESP_ERR_NOT_FOUND)
    {
        return err; /* otros errores: dejar que el HAL decida */
    }

    return ESP_OK;
}

static esp_err_t wifi_tcp_send_bounded(int sock, const uint8_t *data, uint16_t len,
                                       uint32_t timeout_ms)
{
    struct timeval tv = {
        .tv_sec  = (timeout_ms / 1000),
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t sent = 0;
    while (sent < len)
    {
        const int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ESP_LOGW(TAG, "send() fallo: errno=%d", errno);
            return ESP_ERR_TIMEOUT;
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

esp_err_t wifi_tcp_send_endpoint(const robot_endpoint_t *ep, const char *payload)
{
    if (ep == NULL || payload == NULL || payload[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    char ip[16];
    uint16_t port = 0;
    if (!wifi_tcp_parse_endpoint(ep, ip, &port))
    {
        ESP_LOGW(TAG, "Endpoint invalido: '%s'", ep->endpoint);
        return ESP_ERR_INVALID_ARG;
    }

    int sock = -1;
    esp_err_t err = wifi_tcp_connect_bounded(ip, port,
                                             CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS, &sock);
    if (err != ESP_OK)
    {
        return err;
    }

    err = wifi_tcp_send_bounded(sock, (const uint8_t *)payload,
                                (uint16_t)strlen(payload),
                                CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS);
    close(sock);
    return err;
}

static esp_err_t wifi_tcp_init(robot_driver_t *drv)
{
    (void)drv;
    return ESP_OK;
}

static esp_err_t wifi_tcp_probe(robot_driver_t *drv,
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

static robot_result_code_t wifi_tcp_map_error(esp_err_t err)
{
    switch (err)
    {
    case ESP_ERR_TIMEOUT:        return ROBOT_RESULT_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG:    return ROBOT_RESULT_ERR_INVALID_ARG;
    case ESP_ERR_NOT_SUPPORTED:  return ROBOT_RESULT_ERR_UNSUPPORTED;
    case ESP_ERR_INVALID_STATE:  return ROBOT_RESULT_ERR_OFFLINE;
    default:                     return ROBOT_RESULT_ERR_TRANSPORT;
    }
}

static const char *wifi_tcp_payload_for(robot_action_id_t action)
{
    for (size_t i = 0; i < sizeof(s_tcp_payloads) / sizeof(s_tcp_payloads[0]); i++)
    {
        if (s_tcp_payloads[i].action == action)
        {
            return s_tcp_payloads[i].payload;
        }
    }
    return NULL;
}

typedef struct
{
    char ip[16];
    uint16_t port;
    uint32_t delay_ms;
    uint8_t stop_data[WIFI_TCP_STOP_DATA_MAX];
    uint16_t stop_len;
} wifi_tcp_pulse_stop_param_t;

static void wifi_tcp_pulse_stop_task(void *arg)
{
    wifi_tcp_pulse_stop_param_t *p = (wifi_tcp_pulse_stop_param_t *)arg;
    if (p != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(p->delay_ms));
        ESP_LOGI(TAG, "Impulso de %lu ms completado. Enviando STOP por TCP...",
                 (unsigned long)p->delay_ms);

        int sock = -1;
        esp_err_t err = wifi_tcp_connect_bounded(p->ip, p->port,
                                                 CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS, &sock);
        if (err == ESP_OK)
        {
            wifi_tcp_send_bounded(sock, p->stop_data, p->stop_len,
                                  CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS);
            close(sock);
        }
        free(p);
    }
    vTaskDelete(NULL);
}

static esp_err_t wifi_tcp_pulse_stop(const char *ip, uint16_t port, uint32_t delay_ms,
                                     const uint8_t *stop_data, uint16_t stop_len)
{
    if (stop_data == NULL || stop_len == 0 || stop_len > WIFI_TCP_STOP_DATA_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_tcp_pulse_stop_param_t *param = heap_caps_malloc(
        sizeof(wifi_tcp_pulse_stop_param_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (param == NULL)
    {
        param = malloc(sizeof(wifi_tcp_pulse_stop_param_t)); /* Fallback a DRAM */
    }
    if (param == NULL)
    {
        ESP_LOGE(TAG, "pulse_stop: sin memoria para parametros");
        return ESP_ERR_NO_MEM;
    }

    strlcpy(param->ip, ip, sizeof(param->ip));
    param->port = port;
    param->delay_ms = delay_ms;
    param->stop_len = stop_len;
    memcpy(param->stop_data, stop_data, stop_len);

    const BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(wifi_tcp_pulse_stop_task,
                                                          "tcp_pulse_gen",
                                                          WIFI_TCP_PULSE_TASK_STACK,
                                                          param, 5, NULL, 1,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS)
    {
        ESP_LOGE(TAG, "pulse_stop: no se pudo crear la tarea");
        free(param);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t wifi_tcp_execute(robot_driver_t *drv,
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

    if ((WIFI_TCP_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), WIFI_TCP_PROFILE_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *payload = wifi_tcp_payload_for(action);
    if (payload == NULL)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Sin payload configurado para '%s'", robot_action_to_string(action));
        return ESP_ERR_NOT_SUPPORTED;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        out->code = ROBOT_RESULT_ERR_NOT_FOUND;
        snprintf(out->detail, sizeof(out->detail), "Dispositivo '%s' no registrado", alias);
        return ESP_ERR_NOT_FOUND;
    }

    char ip[16];
    uint16_t port = 0;
    if (!wifi_tcp_parse_endpoint(&dev->endpoint, ip, &port))
    {
        out->code = ROBOT_RESULT_ERR_INVALID_ARG;
        snprintf(out->detail, sizeof(out->detail),
                 "Endpoint '%s' invalido (esperado ip:puerto)", dev->endpoint.endpoint);
        return ESP_ERR_INVALID_ARG;
    }

    int sock = -1;
    esp_err_t err = wifi_tcp_connect_bounded(ip, port,
                                             CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS, &sock);
    if (err != ESP_OK)
    {
        out->code = wifi_tcp_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "TCP '%s' (%s:%u): %s", alias, ip, port, esp_err_to_name(err));
        return err;
    }

    err = wifi_tcp_send_bounded(sock, (const uint8_t *)payload, (uint16_t)strlen(payload),
                                CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS);
    close(sock);
    if (err != ESP_OK)
    {
        out->code = wifi_tcp_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "Envio TCP fallido a '%s': %s", alias, esp_err_to_name(err));
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    strlcpy(out->detail, "Comando TCP ejecutado correctamente", sizeof(out->detail));

    const uint32_t duration_ms = params ? params->duration_ms : 0;
    if (action != ROBOT_ACTION_STOP && duration_ms > 0)
    {
        wifi_tcp_pulse_stop(ip, port, duration_ms, (const uint8_t *)"S", 1);
    }

    return ESP_OK;
}

static const robot_driver_t s_wifi_tcp_driver = {
    .profile_id = WIFI_TCP_PROFILE_ID,
    .category   = ROBOT_CATEGORY_GENERIC,
    .protocol   = ROBOT_PROTOCOL_WIFI,
    .capabilities = WIFI_TCP_CAP_MASK,
    .init       = wifi_tcp_init,
    .execute    = wifi_tcp_execute,
    .probe      = wifi_tcp_probe,
    .deinit     = NULL,
    .priv       = NULL,
};

const robot_driver_t *wifi_tcp_get_driver(void)
{
    return &s_wifi_tcp_driver;
}
