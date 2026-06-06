#include "http_client.h"
#include "esp_crt_bundle.h" // Incluir el CRT bundle
#include <esp_log.h>
#include <string.h>
#include <stdlib.h>
#include "settings.h"

static const char *TAG = "HTTP_CLIENT";

// Callback por defecto para manejar eventos HTTP
static esp_err_t default_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->user_data && evt->data && evt->data_len > 0)
        {
            char **buffer_ptr = (char **)evt->user_data;
            size_t new_size = (*buffer_ptr ? strlen(*buffer_ptr) : 0) + evt->data_len + 1;
            char *new_buffer = (char *)realloc(*buffer_ptr, new_size);
            if (!new_buffer)
            {
                ESP_LOGE(TAG, "Error expandiendo buffer HTTP");
                free(*buffer_ptr);
                *buffer_ptr = NULL;
                return ESP_ERR_NO_MEM;
            }
            *buffer_ptr = new_buffer;
            memcpy(*buffer_ptr + new_size - evt->data_len - 1, evt->data, evt->data_len);
            (*buffer_ptr)[new_size - 1] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        ESP_LOGD(TAG, "Unhandled HTTP event: %d", evt->event_id);
        break;
    }
    return ESP_OK;
}

esp_err_t http_client_request(http_request_t *request, http_response_t *response)
{
    if (!request || !request->url)
    {
        ESP_LOGE(TAG, "Solicitud HTTP inválida");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = request->url,
        .timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 40000,
        .event_handler = default_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    char *buffer = NULL;
    config.user_data = &buffer;

    // Inicializar el cliente HTTP
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Error iniciando cliente HTTP");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, request->method);
    if ((request->method == HTTP_METHOD_POST || request->method == HTTP_METHOD_PUT) && request->payload)
    {
        esp_http_client_set_post_field(client, request->payload, strlen(request->payload));
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth_header[256];
    if (!request->api_key)
    {
        ESP_LOGE(TAG, "No se proporcionó API Key para la solicitud HTTP.");
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(auth_header, sizeof(auth_header), "Bearer %s", request->api_key) >= sizeof(auth_header))
    {
        ESP_LOGE(TAG, "Error generando cabecera Authorization");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    if (request->headers)
    {
        char *colon = strchr(request->headers, ':');
        if (colon)
        {
            size_t key_len = colon - request->headers;
            if (key_len < 64)
            {
                char key[64];
                memcpy(key, request->headers, key_len);
                key[key_len] = '\0';
                char *value = colon + 1;
                while (*value == ' ')
                    value++;
                esp_http_client_set_header(client, key, value);
            }
            else
            {
                ESP_LOGE(TAG, "Clave de cabecera demasiado larga");
                esp_http_client_cleanup(client);
                return ESP_ERR_INVALID_ARG;
            }
        }
        else
        {
            ESP_LOGW(TAG, "Formato de cabecera inválido");
        }
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        response->status_code = esp_http_client_get_status_code(client);
        response->response = buffer;
    }
    else
    {
        ESP_LOGE(TAG, "Error en la solicitud HTTP: %s", esp_err_to_name(err));
        free(buffer);
    }

    esp_http_client_cleanup(client);
    return err;
}
