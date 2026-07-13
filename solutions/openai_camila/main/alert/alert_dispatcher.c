#include "alert_dispatcher.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define ALERT_DISPATCHER_TIMEOUT_MS 2500

static const char *TAG = "ALERT_DISPATCHER";
static const char *ALERT_DISPATCHER_URL =
    "https://script.google.com/macros/s/AKfycbyx0Gjd_-1X3ZN_ulQOpHXLg9MVtYdpauGV224B_ae-MohfeNlh6guG6SCy4H46sX0W/exec";

static esp_err_t alert_http_event_handler(esp_http_client_event_t *evt)
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
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        esp_http_client_set_redirection(evt->client);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled HTTP event: %d", evt->event_id);
        break;
    }

    return ESP_OK;
}

esp_err_t alert_dispatcher_send_alert(uint32_t timestamp_ms, float corr_drop)
{
    char payload[96];
    int payload_len = snprintf(payload,
                               sizeof(payload),
                               "{\"timestamp\":\"%" PRIu32 "\",\"corr_drop\":\"%.4f\"}",
                               timestamp_ms,
                               corr_drop);
    if (payload_len < 0 || payload_len >= (int)sizeof(payload))
    {
        ESP_LOGE(TAG, "Alert payload buffer too small");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = ALERT_DISPATCHER_URL,
        .timeout_ms = ALERT_DISPATCHER_TIMEOUT_MS,
        .event_handler = alert_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize alert HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;

    err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set alert HTTP method: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set alert Content-Type: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_http_client_set_post_field(client, payload, payload_len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set alert payload: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Alert webhook status=%d", status_code);
        if (status_code < 200 || status_code >= 300)
        {
            err = ESP_FAIL;
        }
    }
    else
    {
        int status_code = esp_http_client_get_status_code(client);
        if (err == ESP_ERR_HTTP_INCOMPLETE_DATA &&
            (status_code == 200 || status_code == 302))
        {
            ESP_LOGI(TAG,
                     "Alert webhook fire-and-forget accepted despite incomplete data: status=%d",
                     status_code);
            err = ESP_OK;
        }
        else
        {
            ESP_LOGE(TAG, "Alert webhook request failed: %s (status=%d)",
                     esp_err_to_name(err),
                     status_code);
        }
    }

cleanup:
    esp_http_client_cleanup(client);
    return err;
}
