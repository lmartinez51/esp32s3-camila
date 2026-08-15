/**
 * @file wifi_http.c
 * @brief HTTP REST driver for LAN robots (Phase 4).
 *
 * Probe = bounded TCP connect to the HTTP server (shared wifi_tcp helper),
 * fast-fail within CONFIG_ROBOT_PROBE_TIMEOUT_MS. Execute = POST JSON
 * command to http://<ip>:<port><path> via esp_http_client, bounded by
 * CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "wifi_http.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "wifi_tcp.h"
#include "robot_hal.h"
#include <cJSON.h>

#define TAG "WIFI_HTTP"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif
#if !defined(CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS)
#define CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS 1500
#endif
#if !defined(CONFIG_ROBOT_HTTP_COMMAND_PATH)
#define CONFIG_ROBOT_HTTP_COMMAND_PATH "/command"
#endif

#define WIFI_HTTP_CAP_MASK ( \
    (1u << ROBOT_ACTION_FORWARD) | \
    (1u << ROBOT_ACTION_BACKWARD) | \
    (1u << ROBOT_ACTION_LEFT) | \
    (1u << ROBOT_ACTION_RIGHT) | \
    (1u << ROBOT_ACTION_STOP))

static esp_err_t wifi_http_init(robot_driver_t *drv)
{
    (void)drv;
    return ESP_OK;
}

static esp_err_t wifi_http_probe(robot_driver_t *drv,
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

    /* Un TCP connect al servidor HTTP prueba alcanzabilidad en <500 ms. */
    return wifi_tcp_probe_tcp(ip, port, CONFIG_ROBOT_PROBE_TIMEOUT_MS, present);
}

static esp_err_t wifi_http_execute(robot_driver_t *drv,
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

    if ((WIFI_HTTP_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), WIFI_HTTP_PROFILE_ID);
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

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u%s", ip, port, CONFIG_ROBOT_HTTP_COMMAND_PATH);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "Sin memoria para cuerpo JSON");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "action", robot_action_to_string(action));
    if (params != NULL)
    {
        if (params->speed > 0)
        {
            cJSON_AddNumberToObject(root, "speed", (double)params->speed);
        }
        if (params->duration_ms > 0)
        {
            cJSON_AddNumberToObject(root, "duration_ms", (double)params->duration_ms);
        }
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "Sin memoria para serializar JSON");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS,
        .buffer_size    = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        free(body);
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "No se pudo inicializar el cliente HTTP");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    ESP_LOGI(TAG, "HTTP POST %s: %s", url, body);
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status < 200 || status >= 300)
    {
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail),
                 "HTTP '%s' (%s) fallo: %s status=%d", alias,
                 robot_action_to_string(action),
                 (err != ESP_OK) ? esp_err_to_name(err) : "HTTP no-2xx", status);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    out->code = ROBOT_RESULT_OK;
    strlcpy(out->detail, "Comando HTTP ejecutado correctamente", sizeof(out->detail));
    return ESP_OK;
}

static const robot_driver_t s_wifi_http_driver = {
    .profile_id = WIFI_HTTP_PROFILE_ID,
    .category   = ROBOT_CATEGORY_GENERIC,
    .protocol   = ROBOT_PROTOCOL_WIFI,
    .capabilities = WIFI_HTTP_CAP_MASK,
    .init       = wifi_http_init,
    .execute    = wifi_http_execute,
    .probe      = wifi_http_probe,
    .deinit     = NULL,
    .priv       = NULL,
};

const robot_driver_t *wifi_http_get_driver(void)
{
    return &s_wifi_http_driver;
}
