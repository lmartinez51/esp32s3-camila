/* OpenAI signaling

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "https_client.h"
#include "common.h"
#include "esp_log.h"
#include "app_events.h"

#define TAG "OPENAI_SIGNALING"

#define OPENAI_REALTIME_MODEL "gpt-realtime-2"
#define OPENAI_REALTIME_URL "https://api.openai.com/v1/realtime/calls"
#define OPENAI_MULTIPART_BOUNDARY "----esp32-openai-realtime-boundary"
#define OPENAI_SESSION_CONFIG "{\"type\":\"realtime\",\"model\":\"" OPENAI_REALTIME_MODEL "\",\"audio\":{\"output\":{\"voice\":\"ash\"}}}"

#define SAFE_FREE(p) \
    if (p)           \
    {                \
        free(p);     \
        p = NULL;    \
    }

typedef struct
{
    esp_peer_signaling_cfg_t cfg;
    uint8_t *remote_sdp;
    int remote_sdp_size;
    bool stopping;
} openai_signaling_t;

static char *build_realtime_call_body(const char *sdp, size_t sdp_len)
{
    const char *prefix =
        "--" OPENAI_MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"sdp\"\r\n"
        "Content-Type: application/sdp\r\n\r\n";
    const char *middle =
        "\r\n--" OPENAI_MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"session\"\r\n"
        "Content-Type: application/json\r\n\r\n"
        OPENAI_SESSION_CONFIG;
    const char *suffix =
        "\r\n--" OPENAI_MULTIPART_BOUNDARY "--\r\n";

    size_t body_len = strlen(prefix) + sdp_len + strlen(middle) + strlen(suffix);
    char *body = calloc(1, body_len + 1);
    if (!body)
    {
        return NULL;
    }

    char *cursor = body;
    memcpy(cursor, prefix, strlen(prefix));
    cursor += strlen(prefix);
    memcpy(cursor, sdp, sdp_len);
    cursor += sdp_len;
    memcpy(cursor, middle, strlen(middle));
    cursor += strlen(middle);
    memcpy(cursor, suffix, strlen(suffix));

    return body;
}

static int openai_signaling_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    openai_signaling_t *sig = (openai_signaling_t *)calloc(1, sizeof(openai_signaling_t));
    if (sig == NULL)
    {
        return ESP_PEER_ERR_NO_MEM;
    }
    sig->cfg = *cfg;
    *h = sig;
    esp_peer_signaling_ice_info_t ice_info = {
        .is_initiator = true,
    };
    sig->cfg.on_ice_info(&ice_info, sig->cfg.ctx);
    sig->cfg.on_connected(sig->cfg.ctx);
    return ESP_PEER_ERR_NONE;
}

static void openai_sdp_answer(http_resp_t *resp, void *ctx)
{
    openai_signaling_t *sig = (openai_signaling_t *)ctx;
    if (sig == NULL || sig->stopping)
    {
        return;
    }
    // ============================================================================
    // DETECCIÓN DE ERROR 401/403 - SOLUCIÓN DEFINITIVA
    // ============================================================================

    // Verificar si la respuesta contiene un error de autenticación.
    // resp->data no está garantizado como string terminado en NULL.
    if (resp->data && resp->size > 0)
    {
        if (strnstr(resp->data, "\"error\"", resp->size) != NULL)
        {
            ESP_LOGE(TAG, "❌ Error en respuesta de OpenAI API");
            ESP_LOGE(TAG, "Respuesta: %.*s", resp->size, resp->data);

            if (strnstr(resp->data, "insufficient_quota", resp->size) != NULL)
            {
                ESP_LOGE(TAG, "🚫 Error de cuota detectado");
                orchestrator_post_fatal_error();
            }
            else if (strnstr(resp->data, "invalid_api_key", resp->size) != NULL ||
                     strnstr(resp->data, "incorrect_api_key", resp->size) != NULL ||
                     strnstr(resp->data, "Unauthorized", resp->size) != NULL)
            {
                ESP_LOGE(TAG, "🔑 Error de autenticación detectado - API Key inválida");
                orchestrator_post_fatal_error();
            }
            else
            {
                ESP_LOGE(TAG, "❌ Otro rechazo de la API detectado");
                orchestrator_post_fatal_error();
            }

            return;
        }
    }

    // ============================================================================
    // FIN DE LA DETECCIÓN
    // ============================================================================

    SAFE_FREE(sig->remote_sdp);
    sig->remote_sdp = (uint8_t *)malloc(resp->size);
    if (sig->remote_sdp == NULL)
    {
        ESP_LOGE(TAG, "No enough memory for remote sdp");
        return;
    }
    memcpy(sig->remote_sdp, resp->data, resp->size);
    sig->remote_sdp_size = resp->size;
}

static int openai_signaling_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg)
{
    openai_signaling_t *sig = (openai_signaling_t *)h;
    if (sig == NULL || msg == NULL || sig->stopping)
    {
        return 0;
    }

    if (msg->type == ESP_PEER_SIGNALING_MSG_BYE)
    {
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_SDP)
    {
        ESP_LOGD(TAG, "Receive local SDP");
        char content_type[96] = "Content-Type: multipart/form-data; boundary=" OPENAI_MULTIPART_BOUNDARY;
        int len = strlen("Authorization: Bearer ") + strlen((char *)sig->cfg.extra_cfg) + 1;
        char auth[len];
        snprintf(auth, len, "Authorization: Bearer %s", (char *)sig->cfg.extra_cfg);
        char *header[] = {
            content_type,
            auth,
            NULL,
        };
        size_t sdp_len = msg->size > 0 ? msg->size : strlen((const char *)msg->data);
        char *body = build_realtime_call_body((const char *)msg->data, sdp_len);
        if (!body)
        {
            ESP_LOGE(TAG, "No hay memoria para construir multipart de Realtime GA");
            return -1;
        }

        int ret = https_post(OPENAI_REALTIME_URL, header, body, openai_sdp_answer, h);
        free(body);
        if (sig->stopping)
        {
            return 0;
        }
        if (ret != 0 || sig->remote_sdp == NULL)
        {
            ESP_LOGE(TAG, "Fail to post data to %s", OPENAI_REALTIME_URL);
            // ============================================================================
            // DETECCIÓN ADICIONAL: Si https_post falla completamente
            // ============================================================================
            if (ret != 0)
            {
                ESP_LOGE(TAG, "Error en HTTPS POST (código: %d). Se reintentará con la misma llave.", ret);
            }
            // ============================================================================
            return -1;
        }
        esp_peer_signaling_msg_t sdp_msg = {
            .type = ESP_PEER_SIGNALING_MSG_SDP,
            .data = sig->remote_sdp,
            .size = sig->remote_sdp_size,
        };
        sig->cfg.on_msg(&sdp_msg, sig->cfg.ctx);
    }
    return 0;
}

static int openai_signaling_stop(esp_peer_signaling_handle_t h)
{
    openai_signaling_t *sig = (openai_signaling_t *)h;
    if (sig == NULL)
    {
        return 0;
    }
    sig->stopping = true;
    SAFE_FREE(sig->remote_sdp);
    SAFE_FREE(sig);
    return 0;
}

const esp_peer_signaling_impl_t *esp_signaling_get_openai_signaling(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = openai_signaling_start,
        .send_msg = openai_signaling_send_msg,
        .stop = openai_signaling_stop,
    };
    return &impl;
}
