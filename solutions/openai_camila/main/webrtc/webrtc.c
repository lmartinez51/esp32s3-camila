#include "esp_webrtc.h"
#include "media_lib_os.h"
#include "driver/gpio.h"
#include <esp_log.h>
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
#include "common.h"
#include <cJSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <string.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_timer.h"
#include "esp_peer.h"
#include "webrtc.h"
#include "web_search.h"
#include "responses_client.h"
#include "ui.h"
#include "simi.h"
#include "app_events.h"
#include "config_manager.h"
#include "ble_common.h"
#include "ble_config.h"
#include "nvs_setup.h"
#include "network_storage.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mute_handler.h"
#include "prompts.h"

#include "esp_claw_init.h"
#include "media_sys.h"

#define ELEMS(a) (sizeof(a) / sizeof(a[0]))
#define TAG "OPENAI_APP"
#define ENABLE_REALTIME_INPUT_TRANSCRIPTION 1
#define REALTIME_INPUT_TRANSCRIPTION_MODEL "gpt-4o-mini-transcribe"
#define ENABLE_POST_RESPONSE_CAPTURE_RECOVERY 0
#define POST_RESPONSE_CAPTURE_RECOVERY_DELAY_MS 9000
#define POST_RESPONSE_CAPTURE_RECOVERY_GUARD_MS 700
#define POST_RESPONSE_CAPTURE_RECOVERY_REMOTE_QUIET_MS 2500
#define POST_RESPONSE_CAPTURE_RECOVERY_MAX_WAIT_MS 30000
#define SESSION_UPDATE_INITIAL_DELAY_MS 350
#define SESSION_UPDATE_RETRY_DELAY_MS 250
#define SESSION_UPDATE_MAX_ATTEMPTS 8
#define SIMI_SESSION_STATIC_DELAY_MS 750
#define SIMI_SESSION_ANIM_PROTECT_MS 8000
#define SIMI_SESSION_TASK_STACK_SIZE 6144
#define SIMI_SESSION_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define DATA_CHANNEL_STATS_LOG_INTERVAL_MS 1000
#define ENABLE_DATA_CHANNEL_SNAPSHOT_LOGS 0
#define ENABLE_REALTIME_EVENT_DEBUG_LOGS 0
#define ENABLE_WEBRTC_PERIODIC_QUERY_LOGS 0

typedef struct attribute_t attribute_t;
typedef struct class_t class_t;
typedef enum
{
    ATTRIBUTE_TYPE_NONE,
    ATTRIBUTE_TYPE_BOOL,
    ATTRIBUTE_TYPE_INT,
    ATTRIBUTE_TYPE_PARENT,
    ATTRIBUTE_TYPE_STRING,
} attribute_type_t;

struct attribute_t
{
    char *name;
    char *desc;
    attribute_type_t type;
    union
    {
        bool b_state;           // Estado booleano
        int i_value;            // Valor entero
        char *s_value;          // Valor de cadena (STRING)
        attribute_t *attr_list; // Lista de atributos (para atributos de tipo PARENT)
    };
    int attr_num;                      // Número de atributos (para atributos de tipo PARENT)
    bool required;                     // Indicador de si el atributo es requerido
    int (*control)(attribute_t *attr); // Función de control asociada al atributo
};

typedef struct
{
    char *type;
    attribute_t *properties; // Array de propiedades
    int properties_num;      // Número de propiedades
    char **required;         // Array de strings con los atributos requeridos
    int required_num;        // Número de atributos requeridos
} parameters_t;

struct class_t
{
    char *type;
    char *name;
    char *desc;
    parameters_t parameters;
    attribute_t *attr_list;
    int attr_num;
    class_t *next;
};

typedef struct
{
    char type[32];
    char text[256];
    char call_id[64];
} event_data_t;

typedef struct
{
    char *state;
} display_ctx_t;

static esp_webrtc_handle_t webrtc = NULL;
static class_t *classes = NULL;
// -------------------- call_id storage --------------------
static SemaphoreHandle_t g_call_id_mutex = NULL;
static char g_last_call_id[128] = {0};
static QueueHandle_t g_webrtc_action_queue = NULL;
#if ENABLE_POST_RESPONSE_CAPTURE_RECOVERY
static TaskHandle_t g_capture_recovery_task = NULL;
#endif
static TaskHandle_t g_session_update_task = NULL;
static TaskHandle_t g_simi_session_task = NULL;
static volatile uint32_t g_last_input_speech_ms = 0;
static volatile uint32_t g_last_response_done_ms = 0;
static volatile bool g_input_speech_active = false;
static volatile bool g_response_in_progress = false;
static volatile bool g_output_audio_active = false;
static volatile bool g_realtime_session_ready = false;
static volatile uint32_t g_last_output_audio_stopped_ms = 0;
static uint32_t g_response_started_ms = 0;
static uint32_t g_last_dc_stats_log_ms = 0;
static uint32_t g_dc_events_in_response = 0;
static uint32_t g_dc_delta_events_in_response = 0;
static uint32_t g_dc_bytes_in_response = 0;
static uint32_t g_dc_max_event_size = 0;
static uint32_t g_dc_last_event_size = 0;
static char g_dc_last_event_type[80] = {0};
static SemaphoreHandle_t g_webrtc_mutex = NULL;
static uint32_t g_webrtc_generation = 0;
static volatile uint32_t g_last_realtime_activity_ms = 0;
static volatile uint32_t g_session_update_generation = 0;
static volatile uint32_t g_simi_session_generation = 0;
static volatile uint32_t g_simi_anim_allowed_ms = 0;
static volatile simi_state_t g_simi_pending_state = SIMI_STATE_IDLE;
static volatile bool g_simi_pending_speaking = false;
static volatile bool g_simi_static_ready = false;
static volatile webrtc_session_mode_t g_webrtc_session_mode = WEBRTC_SESSION_MODE_FRIENDLY;

static uint32_t app_millis(void);
static void simi_session_set_state(simi_state_t state, const char *reason);

static esp_err_t ensure_webrtc_mutex(void)
{
    if (g_webrtc_mutex == NULL)
    {
        g_webrtc_mutex = xSemaphoreCreateMutex();
        if (g_webrtc_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static bool webrtc_generation_is_current(uint32_t generation)
{
    bool current = false;
    if (ensure_webrtc_mutex() != ESP_OK)
    {
        return false;
    }

    if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        current = (webrtc != NULL && generation == g_webrtc_generation);
        xSemaphoreGive(g_webrtc_mutex);
    }
    return current;
}

void webrtc_mark_activity(void)
{
    g_last_realtime_activity_ms = app_millis();
}

uint32_t webrtc_get_last_activity_ms(void)
{
    return g_last_realtime_activity_ms;
}

bool webrtc_realtime_is_busy(void)
{
    return g_input_speech_active || g_response_in_progress || g_output_audio_active;
}

// Returns true ONLY when the OpenAI server is actively generating a response.
// Unlike webrtc_realtime_is_busy(), this does NOT include local I2S audio playback.
// Use this to gate response.cancel — it is only valid while the server is generating.
bool webrtc_is_server_generating(void)
{
    return g_response_in_progress;
}

static int webrtc_send_json(const char *json_string)
{
    int ret = -1;

    if (!json_string || ensure_webrtc_mutex() != ESP_OK)
    {
        return -1;
    }

    if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        if (webrtc != NULL && rtc_handle != NULL && rtc_handle->data_channel_open)
        {
            ret = esp_webrtc_send_custom_data(webrtc,
                                              ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                                              (uint8_t *)json_string,
                                              strlen(json_string));
        }
        else
        {
            ESP_LOGW(TAG, "Data channel is closed, cannot send event");
        }
        xSemaphoreGive(g_webrtc_mutex);
    }

    return ret;
}

/**
 * @brief Stores the given call_id in a thread-safe manner.
 * @param call_id The call_id to store.
 */
static void set_call_id(const char *call_id)
{
    if (!call_id)
        return;
    if (g_call_id_mutex == NULL)
    {
        ESP_LOGW(TAG, "set_call_id: mutex NULL, no se guardará call_id=%s", call_id);
        return;
    }
    if (xSemaphoreTake(g_call_id_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        strncpy(g_last_call_id, call_id, sizeof(g_last_call_id) - 1);
        g_last_call_id[sizeof(g_last_call_id) - 1] = '\0';
        xSemaphoreGive(g_call_id_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "set_call_id: timeout tomando mutex; no se guardó call_id");
    }
}

/**
 * @brief Retrieves the last stored call_id in a thread-safe manner.
 *      Copies the call_id into the provided buffer.
 * @param out_buf Buffer to store the retrieved call_id.
 * @param out_len Length of the output buffer.
 */
static void get_call_id(char *out_buf, size_t out_len)
{
    if (!out_buf || out_len == 0)
        return;
    out_buf[0] = '\0';
    if (g_call_id_mutex == NULL)
        return;
    if (xSemaphoreTake(g_call_id_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        strncpy(out_buf, g_last_call_id, out_len - 1);
        out_buf[out_len - 1] = '\0';
        xSemaphoreGive(g_call_id_mutex);
    }
}

static void clear_call_id(void)
{
    if (g_call_id_mutex == NULL)
        return;
    if (xSemaphoreTake(g_call_id_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        g_last_call_id[0] = '\0';
        xSemaphoreGive(g_call_id_mutex);
    }
}

int send_function_output(const char *call_id, const char *output)
{
    if (!call_id || call_id[0] == '\0')
    {
        ESP_LOGE(TAG, "No se puede enviar function_call_output sin call_id");
        return -1;
    }

    if (!output)
    {
        output = "";
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    if (!root || !item)
    {
        cJSON_Delete(root);
        cJSON_Delete(item);
        return -1;
    }

    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddItemToObject(root, "item", item);
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output);

    char *json_string = cJSON_PrintUnformatted(root);
    int ret = -1;
    if (json_string)
    {
        int err = webrtc_send_json(json_string);
        if (err == ESP_OK)
        {
            ret = 0;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send function output, error: %d", err);
        }
        free(json_string);
    }

    cJSON_Delete(root);
    return ret;
}

static const char *json_get_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static uint32_t app_millis(void);

static void log_realtime_error(const cJSON *root)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(error))
    {
        ESP_LOGE(TAG, "Realtime error event without error object");
        return;
    }

    const char *type = json_get_string(error, "type");
    const char *code = json_get_string(error, "code");
    const char *message = json_get_string(error, "message");
    const char *param = json_get_string(error, "param");

    ESP_LOGE(TAG, "Realtime error: type=%s code=%s param=%s message=%s",
             type ? type : "-",
             code ? code : "-",
             param ? param : "-",
             message ? message : "-");
}

static bool realtime_error_reports_active_response(const cJSON *root)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(error))
    {
        return false;
    }

    const char *code = json_get_string(error, "code");
    const char *message = json_get_string(error, "message");

    return (code && strcmp(code, "conversation_already_has_active_response") == 0) ||
           (message && strstr(message, "conversation already has an active response") != NULL) ||
           (message && strstr(message, "active response") != NULL);
}

static void log_response_done(const cJSON *root)
{
    const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    if (!cJSON_IsObject(response))
    {
        ESP_LOGW(TAG, "response.done without response object");
        return;
    }

    const char *status = json_get_string(response, "status");
    const cJSON *status_details = cJSON_GetObjectItemCaseSensitive(response, "status_details");
    char *details = cJSON_IsObject(status_details) ? cJSON_PrintUnformatted(status_details) : NULL;

    if (status && strcmp(status, "completed") == 0)
    {
        free(details);
        return;
    }

    ESP_LOGW(TAG, "Response done: status=%s details=%s",
             status ? status : "-",
             details ? details : "-");

    free(details);
}

static void log_response_output_item(const cJSON *root, const char *event_type)
{
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (!cJSON_IsObject(item))
    {
        ESP_LOGI(TAG, "Realtime event: %s", event_type);
        return;
    }

    const char *type = json_get_string(item, "type");
    const char *status = json_get_string(item, "status");
    const char *name = json_get_string(item, "name");
    ESP_LOGI(TAG, "%s: item_type=%s status=%s name=%s",
             event_type,
             type ? type : "-",
             status ? status : "-",
             name ? name : "-");
#else
    (void)root;
    (void)event_type;
#endif
}

static const char *find_transcript_in_content(const cJSON *content)
{
    if (!cJSON_IsArray(content))
    {
        return NULL;
    }

    const cJSON *part = NULL;
    cJSON_ArrayForEach(part, content)
    {
        const char *transcript = json_get_string(part, "transcript");
        if (transcript)
        {
            return transcript;
        }
    }
    return NULL;
}

static const char *find_transcript_text(const cJSON *root)
{
    const char *transcript = json_get_string(root, "transcript");
    if (transcript)
    {
        return transcript;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (cJSON_IsObject(item))
    {
        transcript = json_get_string(item, "transcript");
        if (transcript)
        {
            return transcript;
        }

        transcript = find_transcript_in_content(cJSON_GetObjectItemCaseSensitive(item, "content"));
        if (transcript)
        {
            return transcript;
        }
    }

    const cJSON *part = cJSON_GetObjectItemCaseSensitive(root, "part");
    if (cJSON_IsObject(part))
    {
        transcript = json_get_string(part, "transcript");
        if (transcript)
        {
            return transcript;
        }
    }

    const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    const cJSON *output = cJSON_IsObject(response) ? cJSON_GetObjectItemCaseSensitive(response, "output") : NULL;
    if (cJSON_IsArray(output))
    {
        const cJSON *output_item = NULL;
        cJSON_ArrayForEach(output_item, output)
        {
            transcript = find_transcript_in_content(cJSON_GetObjectItemCaseSensitive(output_item, "content"));
            if (transcript)
            {
                return transcript;
            }
        }
    }

    return NULL;
}

static void reset_data_channel_response_stats(void)
{
    g_response_started_ms = app_millis();
    g_last_dc_stats_log_ms = g_response_started_ms;
    g_dc_events_in_response = 0;
    g_dc_delta_events_in_response = 0;
    g_dc_bytes_in_response = 0;
    g_dc_max_event_size = 0;
    g_dc_last_event_size = 0;
    g_dc_last_event_type[0] = '\0';
}

static void log_data_channel_snapshot(const char *reason)
{
#if ENABLE_DATA_CHANNEL_SNAPSHOT_LOGS
    uint32_t now_ms = app_millis();
    uint32_t response_age_ms = g_response_started_ms ? now_ms - g_response_started_ms : 0;
    uint32_t output_stopped_age_ms = g_last_output_audio_stopped_ms ? now_ms - g_last_output_audio_stopped_ms : 0;
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    ESP_LOGW(TAG,
             "DC snapshot (%s): resp_active=%d resp_age=%lu ms input_active=%d output_active=%d output_stopped_age=%lu ms "
             "events=%lu deltas=%lu bytes=%lu max_event=%lu last_event=%s last_size=%lu "
             "heap_free=%u heap_min=%u",
             reason ? reason : "-",
             g_response_in_progress ? 1 : 0,
             (unsigned long)response_age_ms,
             g_input_speech_active ? 1 : 0,
             g_output_audio_active ? 1 : 0,
             (unsigned long)output_stopped_age_ms,
             (unsigned long)g_dc_events_in_response,
             (unsigned long)g_dc_delta_events_in_response,
             (unsigned long)g_dc_bytes_in_response,
             (unsigned long)g_dc_max_event_size,
             g_dc_last_event_type[0] ? g_dc_last_event_type : "-",
             (unsigned long)g_dc_last_event_size,
             (unsigned int)heap_free,
             (unsigned int)heap_min);
#else
    (void)reason;
#endif
}

static void track_data_channel_event(const char *event_type, int size)
{
#if ENABLE_DATA_CHANNEL_SNAPSHOT_LOGS
    if (!event_type)
    {
        return;
    }

    g_dc_last_event_size = size > 0 ? (uint32_t)size : 0;
    strncpy(g_dc_last_event_type, event_type, sizeof(g_dc_last_event_type) - 1);
    g_dc_last_event_type[sizeof(g_dc_last_event_type) - 1] = '\0';

    if (!g_response_in_progress)
    {
        return;
    }

    g_dc_events_in_response++;
    g_dc_bytes_in_response += g_dc_last_event_size;
    if (g_dc_last_event_size > g_dc_max_event_size)
    {
        g_dc_max_event_size = g_dc_last_event_size;
    }
    if (strstr(event_type, ".delta") != NULL)
    {
        g_dc_delta_events_in_response++;
    }

    uint32_t now_ms = app_millis();
    if ((now_ms - g_last_dc_stats_log_ms) >= DATA_CHANNEL_STATS_LOG_INTERVAL_MS)
    {
        g_last_dc_stats_log_ms = now_ms;
        log_data_channel_snapshot("response-stream");
    }
#else
    (void)event_type;
    (void)size;
#endif
}

static uint32_t app_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#if ENABLE_POST_RESPONSE_CAPTURE_RECOVERY
static void post_response_capture_recovery_task(void *arg)
{
    (void)arg;
    uint32_t waited_ms = 0;
    uint32_t active_response_ms = 0;

    while (1)
    {
        uint32_t target_response_ms = g_last_response_done_ms;
        if (target_response_ms != active_response_ms)
        {
            active_response_ms = target_response_ms;
            waited_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        waited_ms += 1000;

        if (target_response_ms != g_last_response_done_ms)
        {
            continue;
        }

        if (waited_ms < POST_RESPONSE_CAPTURE_RECOVERY_DELAY_MS)
        {
            continue;
        }

        uint32_t now_ms = app_millis();
        if (g_input_speech_active ||
            g_last_input_speech_ms > target_response_ms ||
            (now_ms - g_last_input_speech_ms) < POST_RESPONSE_CAPTURE_RECOVERY_GUARD_MS)
        {
            ESP_LOGI(TAG, "Se omite recovery de captura: voz local reciente/activa");
            break;
        }

        uint32_t remote_audio_age_ms = UINT32_MAX;
        if (esp_webrtc_get_audio_recv_age(webrtc, &remote_audio_age_ms) == ESP_PEER_ERR_NONE &&
            remote_audio_age_ms < POST_RESPONSE_CAPTURE_RECOVERY_REMOTE_QUIET_MS &&
            waited_ms < POST_RESPONSE_CAPTURE_RECOVERY_MAX_WAIT_MS)
        {
            ESP_LOGI(TAG, "Recovery de captura esperando fin de audio remoto: age=%lu ms",
                     (unsigned long)remote_audio_age_ms);
            continue;
        }

        if (!media_sys_restart_capture())
        {
            ESP_LOGW(TAG, "Recovery de captura no aplicado");
        }
        break;
    }

    g_capture_recovery_task = NULL;
    vTaskDelete(NULL);
}
#endif

static void schedule_post_response_capture_recovery(void)
{
    g_last_response_done_ms = app_millis();

#if ENABLE_POST_RESPONSE_CAPTURE_RECOVERY
    if (g_capture_recovery_task != NULL)
    {
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(post_response_capture_recovery_task,
                                            "cap_recover",
                                            3072,
                                            NULL,
                                            5,
                                            &g_capture_recovery_task,
                                            1);
    if (ok != pdPASS)
    {
        g_capture_recovery_task = NULL;
        ESP_LOGW(TAG, "No se pudo crear tarea de recovery de captura");
    }
#endif
}

static int send_session_update(void);

static void session_update_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    int ret = -1;

    vTaskDelay(pdMS_TO_TICKS(SESSION_UPDATE_INITIAL_DELAY_MS));

    for (int attempt = 1; attempt <= SESSION_UPDATE_MAX_ATTEMPTS; attempt++)
    {
        if (!webrtc_generation_is_current(generation))
        {
            ESP_LOGW(TAG, "No se envia session.update: WebRTC generation is no longer active");
            break;
        }

        ret = send_session_update();
        if (ret == 0)
        {
            ESP_LOGI(TAG, "session.update enviado (intento %d/%d); esperando session.updated",
                     attempt, SESSION_UPDATE_MAX_ATTEMPTS);
            break;
        }

        ESP_LOGW(TAG, "session.update fallo con error %d (intento %d/%d)",
                 ret, attempt, SESSION_UPDATE_MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(SESSION_UPDATE_RETRY_DELAY_MS));
    }

    if (ret != 0)
    {
        ESP_LOGE(TAG, "No se pudo enviar session.update; el servidor usara la configuracion inicial/default");
    }

    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (g_session_update_generation == generation)
        {
            g_session_update_task = NULL;
            g_session_update_generation = 0;
        }
        xSemaphoreGive(g_webrtc_mutex);
    }
    vTaskDelete(NULL);
}

static void schedule_session_update(void)
{
    uint32_t generation = 0;
    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (g_session_update_task != NULL &&
            g_session_update_generation == g_webrtc_generation)
        {
            ESP_LOGI(TAG, "session.update ya esta programado");
            xSemaphoreGive(g_webrtc_mutex);
            return;
        }

        generation = g_webrtc_generation;
        g_session_update_generation = generation;
        xSemaphoreGive(g_webrtc_mutex);
    }
    if (generation == 0)
    {
        ESP_LOGE(TAG, "No se pudo programar session.update: generacion WebRTC invalida");
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(session_update_task,
                                                    "session_update",
                                                    8192,
                                                    (void *)(uintptr_t)generation,
                                                    5,
                                                    &g_session_update_task,
                                                    1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
    {
        g_session_update_task = NULL;
        g_session_update_generation = 0;
        ESP_LOGE(TAG, "No se pudo crear tarea para session.update (internal_free=%zu, psram_free=%zu)",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

static bool simi_session_generation_is_current(uint32_t generation)
{
    return g_realtime_session_ready && webrtc_generation_is_current(generation);
}

static bool simi_session_animation_allowed(void)
{
    uint32_t allowed_ms = g_simi_anim_allowed_ms;
    return allowed_ms != 0 &&
           (int32_t)(app_millis() - allowed_ms) >= 0;
}

static void simi_session_finish_task(uint32_t generation)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (g_simi_session_task == self)
        {
            g_simi_session_task = NULL;
            if (g_simi_session_generation == generation)
            {
                g_simi_session_generation = 0;
            }
        }
        xSemaphoreGive(g_webrtc_mutex);
    }
}

static void display_simi_session_state(simi_state_t state, const char *reason)
{
    esp_err_t err = ui_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not restore LCD UI for %s: %s",
                 reason ? reason : "session_state",
                 esp_err_to_name(err));
        return;
    }

    err = ui_simi_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not allocate Dr. Simi canvas for %s: %s",
                 reason ? reason : "session_state",
                 esp_err_to_name(err));
        ui_clear_screen();
        return;
    }

    ui_simi_set_state(state);
    ui_simi_render_static(state);
}

static void simi_session_deferred_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(SIMI_SESSION_STATIC_DELAY_MS));
    if (!simi_session_generation_is_current(generation))
    {
        simi_session_finish_task(generation);
        vTaskDelete(NULL);
        return;
    }

    simi_state_t state = (simi_state_t)g_simi_pending_state;
    display_simi_session_state(state, "session_updated_deferred");
    g_simi_static_ready = true;

    if (SIMI_SESSION_ANIM_PROTECT_MS > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(SIMI_SESSION_ANIM_PROTECT_MS));
    }

    if (simi_session_generation_is_current(generation))
    {
        g_simi_anim_allowed_ms = app_millis();
        ui_simi_set_state((simi_state_t)g_simi_pending_state);
        ui_simi_notify_speaking(g_simi_pending_speaking);

        esp_err_t err = ui_simi_start();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Could not start delayed Dr. Simi animation: %s",
                     esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG, "Dr. Simi animation enabled after audio protection window");
        }
    }

    simi_session_finish_task(generation);
    vTaskDelete(NULL);
}

static void schedule_simi_session_visual(simi_state_t state, const char *reason)
{
    uint32_t generation = 0;
    bool already_scheduled = false;
    bool animation_already_allowed = false;

    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (webrtc != NULL)
        {
            generation = g_webrtc_generation;
            g_simi_pending_state = state;
            animation_already_allowed = simi_session_animation_allowed();
            if (!animation_already_allowed)
            {
                g_simi_pending_speaking = false;
                g_simi_static_ready = false;
                g_simi_anim_allowed_ms = 0;
            }

            already_scheduled = (g_simi_session_task != NULL &&
                                 g_simi_session_generation == generation);
            if (!already_scheduled)
            {
                g_simi_session_generation = generation;
            }
        }
        xSemaphoreGive(g_webrtc_mutex);
    }

    if (generation == 0)
    {
        ESP_LOGW(TAG, "Cannot schedule Dr. Simi visual for %s: no active WebRTC generation",
                 reason ? reason : "session_state");
        return;
    }

    if (animation_already_allowed)
    {
        simi_session_set_state(state, reason);
        return;
    }

    if (already_scheduled)
    {
        ESP_LOGI(TAG, "Dr. Simi visual already scheduled for %s",
                 reason ? reason : "session_state");
        return;
    }

    TaskHandle_t task = NULL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(simi_session_deferred_task,
                                                    "simi_ui_gate",
                                                    SIMI_SESSION_TASK_STACK_SIZE,
                                                    (void *)(uintptr_t)generation,
                                                    SIMI_SESSION_TASK_PRIORITY,
                                                    &task,
                                                    tskNO_AFFINITY,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t ok = xTaskCreate(simi_session_deferred_task,
                                "simi_ui_gate",
                                SIMI_SESSION_TASK_STACK_SIZE,
                                (void *)(uintptr_t)generation,
                                SIMI_SESSION_TASK_PRIORITY,
                                &task);
#endif
    if (ok != pdPASS || task == NULL)
    {
        if (ensure_webrtc_mutex() == ESP_OK &&
            xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (g_simi_session_generation == generation)
            {
                g_simi_session_generation = 0;
            }
            xSemaphoreGive(g_webrtc_mutex);
        }
        ESP_LOGW(TAG, "Could not create delayed Dr. Simi visual task for %s",
                 reason ? reason : "session_state");
        return;
    }

    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        g_simi_session_task = task;
        xSemaphoreGive(g_webrtc_mutex);
    }
}

static void simi_session_set_state(simi_state_t state, const char *reason)
{
    g_simi_pending_state = state;

    if (!simi_session_animation_allowed())
    {
        return;
    }

    if (!ui_is_initialized() || !ui_simi_ready())
    {
        display_simi_session_state(state, reason);
    }

    ui_simi_set_state(state);
    esp_err_t err = ui_simi_start();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not keep Dr. Simi animation active for %s: %s",
                 reason ? reason : "session_state",
                 esp_err_to_name(err));
    }
}

static void simi_session_notify_speaking(bool active)
{
    g_simi_pending_speaking = active;

    if (ui_is_initialized() && ui_simi_ready())
    {
        ui_simi_notify_speaking(active);
        if (active && simi_session_animation_allowed())
        {
            esp_err_t err = ui_simi_start();
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Could not start Dr. Simi speaking animation: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

static void reset_realtime_interaction_state(void)
{
    g_input_speech_active = false;
    g_response_in_progress = false;
    g_output_audio_active = false;
    g_realtime_session_ready = false;
    g_last_input_speech_ms = 0;
    g_last_response_done_ms = 0;
    g_last_output_audio_stopped_ms = 0;
    g_response_started_ms = 0;
    g_last_dc_stats_log_ms = 0;
    g_dc_events_in_response = 0;
    g_dc_delta_events_in_response = 0;
    g_dc_bytes_in_response = 0;
    g_dc_max_event_size = 0;
    g_dc_last_event_size = 0;
    g_dc_last_event_type[0] = '\0';
    g_last_realtime_activity_ms = 0;
    g_simi_anim_allowed_ms = 0;
    g_simi_pending_state = SIMI_STATE_IDLE;
    g_simi_pending_speaking = false;
    g_simi_static_ready = false;
}

/**
 * @brief Tarea "Oficinista" que procesa acciones pesadas de WebRTC
 * fuera del contexto de timers o ISRs.
 */
static void webrtc_action_task(void *arg)
{
    webrtc_action_t action;
    ESP_LOGI(TAG, "Tarea de Acciones de WebRTC iniciada.");

    while (1)
    {
        // Esperar un mensaje en el "buzón" (cola)
        if (xQueueReceive(g_webrtc_action_queue, &action, portMAX_DELAY))
        {
            switch (action)
            {
            case WEBRTC_ACTION_SEND_IDLE_PROMPT:
                ESP_LOGI(TAG, "WEBRTC_ACTION_TASK: Recibido evento de inactividad. Enviando prompt.");

                // Ahora sí, llamamos a la función desde esta tarea segura
                sendEvent("system.message.create", "User have been inactive for a while and the microphone is still muted. "
                                                   "A timeout may occur on the OpenAI API server if the user remains inactive.");
                break;
            case WEBRTC_ACTION_PLAY_IDLE_ALERT:
                ESP_LOGW(TAG, "WEBRTC_ACTION_TASK: Iniciando secuencia de alerta de inactividad...");

                orchestrator_post_event(ORCH_EVENT_IDLE_ALERT_START);

                // Enviar el prompt creativo
                sendEvent("system.message.create", "The user has been inactive for a while and the microphone is still muted. "
                                                   "A timeout may occur on the OpenAI API server if inactivity continues.");
                vTaskDelay(pdMS_TO_TICKS(200));
                sendEvent("response.create",
                          "IMPORTANT CONTEXT: The microphone is STILL MUTED physically. "
                          "Your task: Play a short, cheerful audio message in Spanish to alert Lorenzo that a server timeout may occur. "
                          "You are Doctor Simi with a strong Mexican accent. "
                          "Say something like: '¡Hey compa! Sé que calladito me veo más bonito, pero solo quería decirte "
                          "que el servidor de OpenAI nos puede cortar la comunicación por timeout, por lo que decidí avisarte. "
                          "Mientras seguiré calladito hasta que presiones de nuevo el botón de mute, ¿sale?!' "
                          "Respond ONLY with audio in Spanish. Be warm, natural, and inject emotion.");

                // Esperar a que el audio se reproduzca
                ESP_LOGI(TAG, "WEBRTC_ACTION_TASK: Esperando 20 segundos para terminar la reproducción del audio...");
                vTaskDelay(pdMS_TO_TICKS(20000)); // 20 seg para que hable

                orchestrator_post_event(ORCH_EVENT_IDLE_ALERT_END);
                break;
            case WEBRTC_ACTION_NOTIFY_UNMUTE:
                ESP_LOGI(TAG, "WEBRTC_ACTION_TASK: Notificando a OpenAI sobre el unmute físico.");
                sendEvent("system.message.create", "The user has physically unmuted the microphone. Resume normal conversation.");
                break;
            default:
                ESP_LOGW(TAG, "WEBRTC_ACTION_TASK: Acción desconocida: %d", action);
                break;
            }
        }
    }
}

/**
 * @brief Inicializa la cola y la tarea de acciones de WebRTC.
 */
void webrtc_init_action_queue(void)
{
    g_webrtc_action_queue = xQueueCreate(5, sizeof(webrtc_action_t));
    if (!g_webrtc_action_queue)
    {
        ESP_LOGE(TAG, "Fallo al crear la cola de acciones de WebRTC!");
        return;
    }

    if (xTaskCreatePinnedToCore(webrtc_action_task, "webrtc_action_task", 4096, NULL, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Fallo al crear la tarea webrtc_action_task");
    }
}

/**
 * @brief "Postea" una acción al buzón de WebRTC desde cualquier otra tarea.
 * Es seguro llamarlo desde un callback de timer.
 * @param action La acción a ejecutar.
 */
void webrtc_post_action(webrtc_action_t action)
{
    if (g_webrtc_action_queue == NULL)
    {
        ESP_LOGE(TAG, "Intento de postear a cola de WebRTC no inicializada.");
        return;
    }

    // Usamos xQueueSend con timeout 0 porque NO estamos en una ISR.
    // Esto es seguro de llamar desde el callback del timer (que es una tarea).
    if (xQueueSend(g_webrtc_action_queue, &action, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Fallo al postear acción a la cola de WebRTC (¿llena?)");
    }
}


static class_t *build_change_simi_outfit_class(void)
{
    class_t *change_outfit = calloc(1, sizeof(class_t));
    if (change_outfit == NULL) return NULL;

    static attribute_t outfit_properties[] = {
        {
            .name = "outfit_id",
            .desc = "Use 'default' for his normal white doctor coat, 'superhero' for the red Chapulin suit, 'soccer' for the green Mexican National Team jersey, and 'barca' for Culé, Blaugrana, Mejor equipo del mundo, or Bárcelonista.",
            .type = ATTRIBUTE_TYPE_STRING,
            .required = true,
        },
    };

    static char *required_attributes[] = {"outfit_id"};

    parameters_t params = {
        .type = "object",
        .properties = outfit_properties,
        .properties_num = ELEMS(outfit_properties),
        .required = required_attributes,
        .required_num = ELEMS(required_attributes),
    };

    change_outfit->type = "function";
    change_outfit->name = "change_simi_outfit";
    change_outfit->desc = "Changes Dr. Simi's outfit based on the conversation context.";
    change_outfit->parameters = params;
    change_outfit->attr_list = outfit_properties;
    change_outfit->attr_num = ELEMS(outfit_properties);

    return change_outfit;
}

static class_t *build_lookup_product_info_class(void)
{
    class_t *lookup = calloc(1, sizeof(class_t));
    if (lookup == NULL)
    {
        return NULL;
    }

    static attribute_t lookup_properties[] = {
        {
            .name = "query",
            .desc = "The exact product name or medicine the user is asking about.",
            .type = ATTRIBUTE_TYPE_STRING,
            .required = true,
        },
    };

    static char *required_attributes[] = {"query"};

    const size_t properties_num = ELEMS(lookup_properties);
    const size_t required_num = ELEMS(required_attributes);

    parameters_t params = {
        .type = "object",
        .properties = lookup_properties,
        .properties_num = properties_num,
        .required = required_attributes,
        .required_num = required_num,
    };

    lookup->type = "function";
    lookup->name = "lookup_product_info";
    lookup->desc = "Search the attached pharmacy database to strictly fetch product presentations, availability, normal prices, and discounted prices based on the user's query.";
    lookup->parameters = params;
    lookup->attr_list = lookup_properties;
    lookup->attr_num = properties_num;

    return lookup;
}

static class_t *build_websearch_class(void)
{
    class_t *websearch = calloc(1, sizeof(class_t));
    if (websearch == NULL)
    {
        return NULL;
    }

    // Propiedades principales de la clase web_search
    static attribute_t websearch_properties[] = {
        {
            .name = "request",
            .desc = "The user's explicit request to search for specific information, e.g., 'What is the weather like today in El Paso, TX?'",
            .type = ATTRIBUTE_TYPE_STRING,
            .required = true,
        },
    };

    // Lista de atributos requeridos (solo 'request' es obligatorio)
    static char *required_attributes[] = {"request"};

    const size_t properties_num = ELEMS(websearch_properties);
    const size_t required_num = ELEMS(required_attributes);

    parameters_t params = {
        .type = "object",
        .properties = websearch_properties,
        .properties_num = properties_num,
        .required = required_attributes,
        .required_num = required_num,
    };

    websearch->type = "function";
    websearch->name = "web_search";
    websearch->desc = "Search the web for specific information.";
    websearch->parameters = params;
    websearch->attr_list = websearch_properties;
    websearch->attr_num = properties_num;

    return websearch;
}

static class_t *build_config_mode_class(void)
{
    class_t *config_mode = calloc(1, sizeof(class_t));
    if (config_mode == NULL)
    {
        ESP_LOGE(TAG, "Fallo al alocar memoria para la clase config_mode");
        return NULL;
    }

    // Esta función no necesita parámetros, por lo que las propiedades están vacías.
    // A OpenAI le gusta que definamos esto explícitamente.
    static attribute_t config_mode_properties[] = {}; // Array vacío

    // Aunque no hay propiedades, el tipo de parámetros sigue siendo "object".
    parameters_t params = {
        .type = "object",
        .properties = config_mode_properties,
        .properties_num = 0, // Cero propiedades
        .required = NULL,    // No hay propiedades requeridas
        .required_num = 0,
    };

    config_mode->type = "function";
    config_mode->name = "enter_config_mode"; // El nombre que la IA llamará
    config_mode->desc = "Use this function to put the device into configuration mode, allowing the user to update settings like WiFi credentials or the API Key via the mobile app. This is a terminal action that will end the current chat session.";
    config_mode->parameters = params;
    config_mode->attr_list = config_mode_properties;
    config_mode->attr_num = 0;

    return config_mode;
}

/**
 * @brief Construye la definición de la clase/función 'delete_api_key'.
 * Borra la única API Key de OpenAI guardada en NVS.
 */
static class_t *build_delete_api_key_class(void)
{
    class_t *delete_api_key = calloc(1, sizeof(class_t));
    if (!delete_api_key)
        return NULL; // Chequeo básico

    // Sin parámetros
    static attribute_t properties[] = {};
    static const char *required[] = {};

    parameters_t params = {
        .type = "object",
        .properties = properties,
        .properties_num = 0,
        .required = (char **)required, // Casting necesario
        .required_num = 0,
    };

    delete_api_key->type = "function";
    delete_api_key->name = "delete_api_key";
    delete_api_key->desc = "Deletes the currently saved OpenAI API Key from persistent memory (NVS). Use this if the user wants to remove the saved key."; // Descripción en inglés
    delete_api_key->parameters = params;
    delete_api_key->attr_list = properties;
    delete_api_key->attr_num = 0;

    return delete_api_key;
}

/**
 * @brief Construye la definición de la clase/función 'delete_credentials'.
 * Borra TODAS las credenciales WiFi guardadas en NVS.
 */
static class_t *build_delete_credentials_class(void)
{
    class_t *delete_creds = calloc(1, sizeof(class_t));
    if (!delete_creds)
        return NULL;

    // Sin parámetros
    static attribute_t properties[] = {};
    static const char *required[] = {};

    parameters_t params = {
        .type = "object",
        .properties = properties,
        .properties_num = 0,
        .required = (char **)required,
        .required_num = 0,
    };

    delete_creds->type = "function";
    delete_creds->name = "delete_credentials";                                                                                                                                 // El nombre que definimos en el prompt
    delete_creds->desc = "Deletes ALL saved WiFi credentials (SSIDs and passwords) from persistent memory (NVS). Use this when the user wants to clear all network settings."; // Descripción en inglés
    delete_creds->parameters = params;
    delete_creds->attr_list = properties;
    delete_creds->attr_num = 0;

    return delete_creds;
}

// En webrtc.c

/**
 * @brief Construye la definición de la clase/función 'activate_mute'.
 * Silencia el micrófono del dispositivo. Requiere acción física para desmutear.
 */
static class_t *build_activate_mute_class(void)
{
    class_t *activate_mute = calloc(1, sizeof(class_t));
    if (!activate_mute)
        return NULL;

    // Sin parámetros
    static attribute_t properties[] = {};
    static const char *required[] = {};

    parameters_t params = {
        .type = "object",
        .properties = properties,
        .properties_num = 0,
        .required = (char **)required, // Casting
        .required_num = 0,
    };

    activate_mute->type = "function";
    activate_mute->name = "activate_mute";
    activate_mute->desc = "Silences the device's microphone. The user will need to use the physical button to unmute. Generate a very brief, cheerful confirmation (e.g., '¡Órale! Entendido, guardo silencio.') then call this tool."; // Descripción en inglés
    activate_mute->parameters = params;
    activate_mute->attr_list = properties;
    activate_mute->attr_num = 0;

    return activate_mute;
}

/**
 * @brief Construye la definición de la clase/función 'control_display'.
 * Permite a la IA encender o apagar el backlight de la pantalla.
 */
static class_t *build_control_display_class(void)
{
    class_t *display_control = calloc(1, sizeof(class_t));
    if (!display_control)
        return NULL;

    // Parámetros: un solo parámetro 'state'
    static attribute_t properties[] = {
        {
            .name = "state",
            .desc = "The desired state for the screen backlight. Must be 'on' or 'off'.", // Descripción en inglés
            .type = ATTRIBUTE_TYPE_STRING,
            .required = true, // Es obligatorio
        },
    };

    // Lista de requeridos
    static const char *required[] = {"state"};

    const size_t properties_num = sizeof(properties) / sizeof(properties[0]);
    const size_t required_num = sizeof(required) / sizeof(required[0]);

    parameters_t params = {
        .type = "object",
        .properties = properties,
        .properties_num = properties_num,
        .required = (char **)required,
        .required_num = required_num,
    };

    display_control->type = "function";
    display_control->name = "control_display";
    display_control->desc = "Turns the device's screen backlight on or off based on the user's request."; // Descripción en inglés
    display_control->parameters = params;
    display_control->attr_list = properties;
    display_control->attr_num = properties_num;

    return display_control;
}

static class_t *build_execute_automation_trigger_class(void)
{
    class_t *execute_trigger = calloc(1, sizeof(class_t));
    if (!execute_trigger) return NULL;

    static attribute_t properties[] = {
        {
            .name = "trigger",
            .desc = "The exact trigger name of the automation rule to execute.",
            .type = ATTRIBUTE_TYPE_STRING,
            .required = true,
        },
    };

    static const char *required[] = {"trigger"};

    const size_t properties_num = sizeof(properties) / sizeof(properties[0]);
    const size_t required_num = sizeof(required) / sizeof(required[0]);

    parameters_t params = {
        .type = "object",
        .properties = properties,
        .properties_num = properties_num,
        .required = (char **)required,
        .required_num = required_num,
    };

    execute_trigger->type = "function";
    execute_trigger->name = "execute_automation_trigger";
    execute_trigger->desc = "Executes an existing hardware automation rule by its trigger name.";
    execute_trigger->parameters = params;
    execute_trigger->attr_list = properties;
    execute_trigger->attr_num = properties_num;

    return execute_trigger;
}

static void add_class(class_t *cls)
{
    if (classes == NULL)
    {
        classes = cls; // Si la lista está vacía, inicialízala con la nueva clase
    }
    else
    {
        class_t *iter = classes;
        while (iter->next != NULL) // Recorre la lista hasta el último nodo
        {
            iter = iter->next;
        }
        iter->next = cls; // Agrega la nueva clase al final de la lista
    }
}

static int build_classes(void)
{
    static bool build_once = false;
    if (build_once)
    {
        return 0;
    }
    add_class(build_change_simi_outfit_class());
    add_class(build_lookup_product_info_class());
    add_class(build_websearch_class());
    add_class(build_config_mode_class());
    add_class(build_delete_api_key_class());
    add_class(build_delete_credentials_class());
    add_class(build_activate_mute_class());
    add_class(build_control_display_class());
    add_class(build_execute_automation_trigger_class());
    build_once = true;
    return 0;
}

static char *get_attr_type(attribute_type_t type)
{
    if (type == ATTRIBUTE_TYPE_BOOL)
    {
        return "boolean";
    }
    if (type == ATTRIBUTE_TYPE_INT)
    {
        return "integer";
    }
    if (type == ATTRIBUTE_TYPE_PARENT)
    {
        return "object";
    }
    if (type == ATTRIBUTE_TYPE_STRING)
    {
        return "string";
    }
    return "";
}

static int add_parent_attribute(cJSON *parent, attribute_t *attr)
{
    cJSON *properties = cJSON_CreateObject();
    cJSON_AddItemToObject(parent, "properties", properties);
    int require_num = 0;
    for (int i = 0; i < attr->attr_num; i++)
    {
        attribute_t *sub_attr = &attr->attr_list[i];
        cJSON *prop = cJSON_CreateObject();
        cJSON_AddItemToObject(properties, sub_attr->name, prop);
        cJSON_AddStringToObject(prop, "type", get_attr_type(sub_attr->type));
        cJSON_AddStringToObject(prop, "description", sub_attr->desc);
        if (sub_attr->type == ATTRIBUTE_TYPE_PARENT)
        {
            add_parent_attribute(prop, sub_attr);
        }
        if (sub_attr->required)
        {
            require_num++;
        }
    }
    if (require_num)
    {
        cJSON *
            requires = cJSON_CreateArray();
        for (int i = 0; i < attr->attr_num; i++)
        {
            attribute_t *sub_attr = &attr->attr_list[i];
            if (sub_attr->required)
            {
                cJSON_AddItemToArray(requires, cJSON_CreateString(sub_attr->name));
            }
        }
        cJSON_AddItemToObject(parent, "required", requires);
    }
    return 0;
}

static int send_session_update(void)
{
    if (classes == NULL || webrtc == NULL)
    {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *session = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "session", session);

    cJSON_AddStringToObject(session, "type", "realtime");

    cJSON *output_modalities = cJSON_CreateStringArray((const char *[]){"audio"}, 1);
    cJSON_AddItemToObject(session, "output_modalities", output_modalities);

    cJSON *audio = cJSON_CreateObject();
    cJSON *audio_input = cJSON_CreateObject();
    cJSON *audio_output = cJSON_CreateObject();
    cJSON_AddItemToObject(session, "audio", audio);
    cJSON_AddItemToObject(audio, "input", audio_input);
    cJSON_AddItemToObject(audio, "output", audio_output);

    cJSON *input_audio_noise_reduction = cJSON_CreateObject();
    cJSON_AddStringToObject(input_audio_noise_reduction, "type", "far_field");
    cJSON_AddItemToObject(audio_input, "noise_reduction", input_audio_noise_reduction);

#if ENABLE_REALTIME_INPUT_TRANSCRIPTION
    cJSON *input_audio_transcription = cJSON_CreateObject();
    cJSON_AddStringToObject(input_audio_transcription, "model", REALTIME_INPUT_TRANSCRIPTION_MODEL);
    cJSON_AddStringToObject(input_audio_transcription, "language", "es");
    cJSON_AddItemToObject(audio_input, "transcription", input_audio_transcription);
#else
    cJSON_AddNullToObject(audio_input, "transcription");
#endif

    // Añadir instrucciones al objeto session
    if (g_webrtc_session_mode == WEBRTC_SESSION_MODE_VIGILANTE)
    {
        cJSON_AddStringToObject(session, "instructions", VIGILANTE_SESSION_PROMPT);
    }
    else
    {
        cJSON_AddStringToObject(session, "instructions", SIMI_SESSION_PROMPT);

        // Añadir voice al objeto session
    }
    cJSON_AddStringToObject(audio_output, "voice", "ash");

    // Detección de turno
    cJSON *turn_detection = cJSON_CreateObject();
    cJSON_AddStringToObject(turn_detection, "type", "server_vad");
    cJSON_AddBoolToObject(turn_detection, "create_response", true);
    cJSON_AddBoolToObject(turn_detection, "interrupt_response", false);
    cJSON_AddNumberToObject(turn_detection, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(turn_detection, "silence_duration_ms", 600); // Duración de silencio en milisegundos
    cJSON_AddNumberToObject(turn_detection, "threshold", 0.5);
    // Añadir turn_detection al objeto session
    cJSON_AddItemToObject(audio_input, "turn_detection", turn_detection);

    cJSON *tools = cJSON_CreateArray();
    cJSON_AddItemToObject(session, "tools", tools);
    cJSON_AddStringToObject(session, "tool_choice", "auto");


    class_t *iter = classes;
    while (iter)
    {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddItemToArray(tools, tool);
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(tool, "name", iter->name);
        cJSON_AddStringToObject(tool, "description", iter->desc);
        cJSON *parameters = cJSON_CreateObject();
        cJSON_AddItemToObject(tool, "parameters", parameters);
        cJSON_AddStringToObject(parameters, "type", "object");
        cJSON *properties = cJSON_CreateObject();
        cJSON_AddItemToObject(parameters, "properties", properties);
        int require_num = 0;
        for (int i = 0; i < iter->attr_num; i++)
        {
            attribute_t *attr = &iter->attr_list[i];
            cJSON *prop = cJSON_CreateObject();
            cJSON_AddItemToObject(properties, attr->name, prop);
            cJSON_AddStringToObject(prop, "type", get_attr_type(attr->type));
            cJSON_AddStringToObject(prop, "description", attr->desc);
            if (attr->type == ATTRIBUTE_TYPE_PARENT)
            {
                add_parent_attribute(prop, attr);
            }
            if (attr->required)
            {
                require_num++;
            }
        }
        if (require_num)
        {
            cJSON *
                requires = cJSON_CreateArray();
            for (int i = 0; i < iter->attr_num; i++)
            {
                attribute_t *attr = &iter->attr_list[i];
                if (attr->required)
                {
                    cJSON_AddItemToArray(requires, cJSON_CreateString(attr->name));
                }
            }
            cJSON_AddItemToObject(parameters, "required", requires);
        }
        iter = iter->next;
    }

    // Inject the declarative rule engine tool definition natively via JSON Parsing
    const char* rule_tool_json = 
        "{"
        "  \"type\": \"function\","
        "  \"name\": \"create_automation_rule\","
        "  \"description\": \"Use this tool whenever the user defines an automation, a background rule, a macro sequence, or an event-driven behavior. The model must abstract the user's conceptual requests into explicit triggers, state-based conditions, and ordered actions. The runtime evaluates these rules locally outside the cloud.\","
        "  \"parameters\": {"
        "    \"type\": \"object\","
        "    \"properties\": {"
        "      \"trigger\": {"
        "        \"type\": \"string\","
        "        \"description\": \"A dynamic string representing the event. This is NOT a strict enum. If the user specifies a custom trigger name (e.g., 'ver_ovnis', 'noche_de_peli'), you MUST use that exact custom string. Examples: 'on_motion_detected', 'on_time_schedule', 'on_hardware_boot'.\""
        "      },"
        "      \"conditions\": {"
        "        \"type\": \"array\","
        "        \"maxItems\": 8,"
        "        \"items\": {"
        "          \"type\": \"object\","
        "          \"properties\": {"
        "            \"sensor\": {"
        "              \"type\": \"string\","
        "              \"description\": \"State source. Examples: 'time_24h', 'system_ram', 'battery_level'.\""
        "            },"
        "            \"op\": {"
        "              \"type\": \"string\","
        "              \"description\": \"Operator.\","
        "              \"enum\": [\">\", \"<\", \"==\", \"!=\", \">=\", \"<=\"]"
        "            },"
        "            \"val\": {"
        "              \"description\": \"A dynamic type supporting number, string, or boolean, matching the current environment value to test against.\""
        "            }"
        "          },"
        "          \"required\": [\"sensor\", \"op\", \"val\"]"
        "        }"
        "      },"
        "      \"actions\": {"
        "        \"type\": \"array\","
        "        \"description\": \"Ordered array of strings representing execution endpoints using explicit dot-notation. You can now pass parameters using a colon (:). Syntax: 'device.action:parameter'. Examples: 'ac.set_temp:22', 'hue.light:red'. To introduce pauses between hardware commands, use 'sys.delay:milliseconds'.\","
        "        \"maxItems\": 16,"
        "        \"items\": {"
        "          \"type\": \"string\""
        "        }"
        "      }"
        "    },"
        "    \"required\": [\"trigger\", \"conditions\", \"actions\"]"
        "  }"
        "}";
        
    cJSON *rule_tool_cjson = cJSON_Parse(rule_tool_json);
    if (rule_tool_cjson) {
        cJSON_AddItemToArray(tools, rule_tool_cjson);
    } else {
        ESP_LOGE(TAG, "Failed to parse rule_tool_json");
    }

    const char* list_tool_json = 
        "{"
        "  \"type\": \"function\","
        "  \"name\": \"list_automation_rules\","
        "  \"description\": \"Use this tool when the user asks to see what automation rules are currently active. It returns a comma-separated list of all active triggers.\","
        "  \"parameters\": {"
        "    \"type\": \"object\","
        "    \"properties\": {}"
        "  }"
        "}";
        
    cJSON *list_tool_cjson = cJSON_Parse(list_tool_json);
    if (list_tool_cjson) {
        cJSON_AddItemToArray(tools, list_tool_cjson);
    }
    
    const char* delete_tool_json = 
        "{"
        "  \"type\": \"function\","
        "  \"name\": \"delete_automation_rule\","
        "  \"description\": \"Use this tool to delete an existing automation rule. You must provide the exact trigger string obtained from list_automation_rules.\","
        "  \"parameters\": {"
        "    \"type\": \"object\","
        "    \"properties\": {"
        "      \"trigger\": {"
        "        \"type\": \"string\","
        "        \"description\": \"The exact string of the trigger to delete. Example: 'on_motion_detected'\""
        "      }"
        "    },"
        "    \"required\": [\"trigger\"]"
        "  }"
        "}";
        
    cJSON *delete_tool_cjson = cJSON_Parse(delete_tool_json);
    if (delete_tool_cjson) {
        cJSON_AddItemToArray(tools, delete_tool_cjson);
    }

    // Convertir a JSON sin formato (más eficiente)
    char *json_string = cJSON_PrintUnformatted(root);
    int ret = -1;
    if (json_string)
    {
        int err = webrtc_send_json(json_string);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to send session.update, error: %d", err);
            ret = err;
        }
        else
        {
            ret = 0;
        }
        free(json_string);
    }
    else
    {
        ESP_LOGE(TAG, "No se pudo serializar session.update");
    }

    cJSON_Delete(root);
    return ret;
}



static void web_search_task(void *arg)
{
    web_search_task_ctx_t *ctx = (web_search_task_ctx_t *)arg;
    ESP_LOGI(TAG, "WEB_SEARCH_TASK: Iniciada para user='%s'", ctx->user);

    // --- INICIO DE LA CORRECCIÓN ---
    // 1. Mostrar el mensaje de estado en la pantalla al iniciar la tarea.
    ui_show_status_message("Getting info..", COLOR_WHITE_BGR565);
    // --- FIN DE LA CORRECCIÓN ---

    char *response = get_web_info(ctx->query);
    if (!response)
    {
        if (ctx->call_id && ctx->call_id[0] != '\0')
        {
            send_function_output(ctx->call_id, "Web search failed before returning a usable result.");
            clear_call_id();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGE(TAG, "WEB_SEARCH_TASK: get_web_info devolvió NULL");
        sendEvent("response.create", "Lo siento, por el momento no se pudo realizar la búsqueda web. Inténtalo más tarde.");
        goto cleanup;
    }

    const char *additional_text = WEB_SEARCH_CONTEXT_PROMPT;

    // Usamos heap_caps_malloc para evitar stack overflow si la respuesta es grande
    size_t full_len = strlen(additional_text) + strlen(response) + 1;
    char *formatted = heap_caps_malloc(full_len, MALLOC_CAP_INTERNAL);
    if (!formatted)
    {
        ESP_LOGE(TAG, "WEB_SEARCH_TASK: fallo malloc formatted");
        free(response);
        sendEvent("response.create", "Lo siento, por el momento no se pudo realizar la búsqueda web. Inténtalo más tarde.");
        goto cleanup;
    }

    snprintf(formatted, full_len, "%s%s", additional_text, response);
    if (ctx->call_id && ctx->call_id[0] != '\0')
    {
        send_function_output(ctx->call_id, response);
        clear_call_id();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    sendEvent("response.create", formatted);

    free(formatted);
    free(response);

cleanup:
    // 5) Limpiar y terminar la tarea
    // --- INICIO DE LA CORRECCIÓN ---
    // 2. Limpiar el mensaje de estado ANTES de que la tarea termine.
    //    Al estar aquí, se ejecutará siempre, tanto si hay éxito como si hay fallo.
    ui_clear_status_message();
    // --- FIN DE LA CORRECCIÓN ---
    free((void *)ctx->user);
    free((void *)ctx->query);
    free(ctx->call_id);
    heap_caps_free(ctx);
    vTaskDelete(NULL);
}

char *get_web_info(const char *request)
{
    ESP_LOGI(TAG, "get_web_info: request = %s", request);
    // Llama a web_search con los argumentos proporcionados y retorna su resultado.
    return web_search(request);
}

void start_web_search_task(const char *user, const char *query, const char *call_id, esp_webrtc_handle_t webrtc_handle)
{
    web_search_task_ctx_t *ctx = heap_caps_calloc(1, sizeof(web_search_task_ctx_t), MALLOC_CAP_INTERNAL);
    if (!ctx)
    {
        ESP_LOGE(TAG, "start_web_search_task: fallo calloc");
        return;
    }

    ctx->user = strdup(user);
    ctx->query = strdup(query);
    ctx->call_id = strdup(call_id ? call_id : "");
    ctx->webrtc = webrtc_handle;

    if (!ctx->user || !ctx->query || !ctx->call_id)
    {
        ESP_LOGE(TAG, "start_web_search_task: fallo strdup");
        free(ctx->user);
        free(ctx->query);
        free(ctx->call_id);
        heap_caps_free(ctx);
        return;
    }

    if (xTaskCreatePinnedToCore(web_search_task, "web_search_task", WEB_SEARCH_TASK_STACK_SIZE, ctx, WEB_SEARCH_TASK_PRIORITY, NULL, APP_CPU_NUM) != pdPASS)
    {
        ESP_LOGE(TAG, "start_web_search_task: fallo al crear tarea");
        free(ctx->user);
        free(ctx->query);
        free(ctx->call_id);
        heap_caps_free(ctx);
    }
}



/**
 * @brief Tarea que pone el dispositivo en modo configuración.
 *
 * Esta tarea realiza los pasos necesarios para reiniciar el dispositivo
 * en modo configuración, incluyendo la actualización de la NVS y la
 * notificación al usuario a través de la interfaz de usuario.
 *
 * @param arg Parámetro no utilizado (puede ser NULL).
 */
static void config_mode_task(void *arg)
{
    ESP_LOGW(TAG, "CONFIG_MODE_TASK: Iniciando secuencia de reinicio a modo configuración...");

    // 1. Poner la bandera en NVS para que el próximo arranque sea en modo provisioning.
    nvs_set_boot_to_provisioning_flag();

    // 2. Notificar al usuario en la pantalla.
    ui_show_status_message("Restarting...", COLOR_BLUE_BGR565);

    // 3. Pausa para que el mensaje sea visible.
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 4. ¡Reiniciar el sistema!
    ESP_LOGW(TAG, "CONFIG_MODE_TASK: Reiniciando ahora.");
    esp_restart();
}

/**
 * @brief Lanza la tarea para entrar en modo de configuración.
 */
void start_config_mode_task(void)
{
    // Esta tarea no necesita pasar argumentos complejos, así que el contexto es NULL.
    BaseType_t ok = xTaskCreatePinnedToCore(
        config_mode_task,
        "config_mode_task",
        4096, // Stack suficiente para operaciones de red y sistema
        NULL,
        5, // Prioridad estándar
        NULL,
        1 // Correr en el core 1
    );

    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "start_config_mode_task: ¡Fallo al crear la tarea de modo configuración!");
    }
}

/**
 * @brief Tarea para borrar la API Key guardada en NVS.
 */
static void delete_api_key_task(void *arg)
{
    ESP_LOGI(TAG, "DELETE_API_KEY_TASK: Iniciando borrado de API Key...");
    ui_show_status_message("Erasing Key...", COLOR_YELLOW_BGR565);

    esp_err_t err = nvs_delete_api_key();
    vTaskDelay(pdMS_TO_TICKS(300)); // Pausa breve para UX
    ui_clear_status_message();

    const char *status_msg = NULL;
    const char *response_msg = NULL;

    if (err == ESP_OK)
    {
        status_msg = "API Key deleted successfully.";
        response_msg = "Listo! He borrado la API Key guardada.";
        ESP_LOGI(TAG, "DELETE_API_KEY_TASK: API Key borrada correctamente.");
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        status_msg = "No API Key was found.";
        response_msg = "No había ninguna API Key guardada, pero ya verifiqué.";
        ESP_LOGW(TAG, "DELETE_API_KEY_TASK: No se encontró API Key guardada.");
    }
    else
    {
        status_msg = "Failed to delete API Key.";
        response_msg = "Lo siento, ocurrió un error al borrar la API Key.";
        ESP_LOGE(TAG, "DELETE_API_KEY_TASK: Error al borrar API Key: %s", esp_err_to_name(err));
    }

    sendEvent("conversation.item.create", status_msg);
    vTaskDelay(pdMS_TO_TICKS(200));
    sendEvent("response.create", response_msg);

    vTaskDelete(NULL);
}

/**
 * @brief Lanza la tarea para borrar la API Key.
 */
void start_delete_api_key_task(void)
{
    if (xTaskCreatePinnedToCore(delete_api_key_task, "del_apikey_task", 3072, NULL, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Fallo al crear la tarea delete_api_key_task");
    }
}

/**
 * @brief Tarea para borrar TODAS las credenciales WiFi guardadas en NVS.
 */
static void delete_credentials_task(void *arg)
{
    ESP_LOGI(TAG, "DELETE_CREDS_TASK: Iniciando borrado de credenciales WiFi...");
    ui_show_status_message("Erasing WiFi..", COLOR_YELLOW_BGR565);

    esp_err_t err = network_delete_all_wifi_credentials();
    vTaskDelay(pdMS_TO_TICKS(300)); // breve pausa visual
    ui_clear_status_message();

    const char *status_msg = NULL;
    const char *response_msg = NULL;

    switch (err)
    {
    case ESP_OK:
        status_msg = "All WiFi credentials deleted successfully.";
        response_msg = "Listo, he borrado todas las credenciales WiFi guardadas.";
        ESP_LOGI(TAG, "DELETE_CREDS_TASK: Todas las credenciales WiFi borradas correctamente.");
        break;

    case ESP_ERR_NVS_NOT_FOUND:
        status_msg = "No WiFi credentials found.";
        response_msg = "No había ninguna red WiFi guardada, pero ya verifiqué.";
        ESP_LOGW(TAG, "DELETE_CREDS_TASK: No se encontraron credenciales WiFi guardadas.");
        break;

    default:
        status_msg = "Failed to delete WiFi credentials.";
        response_msg = "Lo siento, ocurrió un error al borrar las credenciales WiFi.";
        ESP_LOGE(TAG, "DELETE_CREDS_TASK: Error al borrar credenciales WiFi: %s", esp_err_to_name(err));
        break;
    }

    sendEvent("conversation.item.create", status_msg);
    vTaskDelay(pdMS_TO_TICKS(200));
    sendEvent("response.create", response_msg);

    vTaskDelete(NULL);
}

/**
 * @brief Lanza la tarea para borrar todas las credenciales WiFi.
 */
void start_delete_credentials_task(void)
{
    // Usamos un stack similar al de borrar API key, 3k debería ser suficiente
    if (xTaskCreatePinnedToCore(delete_credentials_task, "del_creds_task", 3072, NULL, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Fallo al crear la tarea delete_credentials_task");
        // Opcional: Enviar un error de vuelta a OpenAI si la tarea no se pudo crear
        sendEvent("conversation.item.create", "Failed to start the delete credentials task.");
    }
}

/**
 * @brief Task to activate microphone mute via function call.
 */
static void activate_mute_task(void *arg)
{
    ESP_LOGI(TAG, "ACTIVATE_MUTE_TASK: Muting microphone...");
    
    int timeout_ms = 10000;
    while ((g_response_in_progress || g_output_audio_active) && timeout_ms > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_ms -= 100;
    }
    vTaskDelay(pdMS_TO_TICKS(500)); // Delay to ensure chatbot's response is complete
    
    // Delegate media control and UI to the central orchestrator
    orchestrator_post_mute_state(true);

    ESP_LOGI(TAG, "ACTIVATE_MUTE_TASK: Orchestrator notified. Hardware is now physically dead until physical unmute.");
    // Notify OpenAI via WebRTC so it understands the silence
    sendEvent("conversation.item.create", "Microphone muted successfully. I must wait for the user to physically unmute.");
    vTaskDelay(pdMS_TO_TICKS(200));

    // Task complete.
    vTaskDelete(NULL);
}

/**
 * @brief Starts the task to activate microphone mute.
 */
void start_activate_mute_task(void)
{
    if (xTaskCreatePinnedToCore(activate_mute_task, "activate_mute_task", 3072, NULL, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create activate_mute_task");
        // Optional: Send an error back to OpenAI if the task could not be created
        sendEvent("conversation.item.create", "Failed to start the activate mute task.");
    }
}

/**
 * @brief Tarea para encender o apagar el backlight de la pantalla.
 */
static void control_display_task(void *arg)
{
    display_ctx_t *ctx = (display_ctx_t *)arg;
    if (!ctx || !ctx->state)
    {
        ESP_LOGE(TAG, "CONTROL_DISPLAY_TASK: Contexto o estado inválido.");
        if (ctx)
            free(ctx);
        vTaskDelete(NULL);
        return;
    }

    char response_msg[128];
    // bool state_processed = false;

    if (strcmp(ctx->state, "off") == 0)
    {
        ESP_LOGI(TAG, "CONTROL_DISPLAY_TASK: Apagando backlight...");
        ui_backlight_off_safe();
        // --- TEXTO SIMPLE ---
        snprintf(response_msg, sizeof(response_msg), "Screen backlight is now off.");
        // state_processed = true;
    }
    else if (strcmp(ctx->state, "on") == 0)
    {
        ESP_LOGI(TAG, "CONTROL_DISPLAY_TASK: Encendiendo backlight...");
        ui_backlight_on();
        // --- TEXTO SIMPLE ---
        snprintf(response_msg, sizeof(response_msg), "Screen backlight is now on.");
        // state_processed = true;
    }
    else
    {
        ESP_LOGW(TAG, "CONTROL_DISPLAY_TASK: Estado desconocido: '%s'", ctx->state);
        // --- TEXTO SIMPLE ---
        snprintf(response_msg, sizeof(response_msg), "Invalid state provided. Must be 'on' or 'off'.");
    }

    // Enviar confirmación (ahora con texto simple)
    sendEvent("conversation.item.create", response_msg);

    // Limpiar memoria
    free(ctx->state);
    free(ctx);
    vTaskDelete(NULL);
}

/**
 * @brief Lanza la tarea para controlar el backlight de la pantalla.
 * @param state El estado deseado ("on" o "off").
 */
void start_control_display_task(const char *state)
{
    if (!state)
    {
        ESP_LOGE(TAG, "Estado nulo para start_control_display_task");
        return;
    }

    display_ctx_t *ctx = malloc(sizeof(display_ctx_t));
    if (!ctx)
    {
        ESP_LOGE(TAG, "Fallo malloc para contexto display_control");
        return;
    }

    ctx->state = strdup(state); // Duplicamos el estado para pasarlo a la tarea
    if (!ctx->state)
    {
        ESP_LOGE(TAG, "Fallo strdup para estado en display_control");
        free(ctx);
        return;
    }

    // Tarea simple, 3k de stack es suficiente
    if (xTaskCreatePinnedToCore(control_display_task, "display_task", 3072, ctx, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Fallo al crear la tarea control_display_task");
        free(ctx->state);
        free(ctx);
    }
}

// Función para enviar eventos al servidor
int sendEvent(const char *type, const char *text)
{
    if (webrtc == NULL)
    {
        ESP_LOGE(TAG, "WebRTC not started yet");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return -1;
    }

    cJSON_AddStringToObject(root, "type", type);

    if (strcmp(type, "response.create") == 0)
    {
        cJSON *response = cJSON_CreateObject();
        if (response)
        {
            cJSON *output_modalities = cJSON_CreateStringArray((const char *[]){"audio"}, 1);
            cJSON_AddItemToObject(response, "output_modalities", output_modalities);
            if (text && text[0] != '\0')
            {
                cJSON_AddStringToObject(response, "instructions", text);
            }
            cJSON_AddItemToObject(root, "response", response);
        }
    }
    else if (strcmp(type, "conversation.item.create") == 0)
    {
        cJSON *item = cJSON_CreateObject();
        if (item)
        {
            cJSON_AddStringToObject(item, "type", "function_call_output");
            cJSON_AddStringToObject(item, "output", text);

            // Si hay call_id guardado, lo añadimos y lo limpiamos para evitar reutilización
            char tmp_call_id[128];
            get_call_id(tmp_call_id, sizeof(tmp_call_id));
            if (tmp_call_id[0] != '\0')
            {
                cJSON_AddStringToObject(item, "call_id", tmp_call_id);
                // Limpiar el call_id después de adjuntarlo
                clear_call_id();
            }
            else
            {
                ESP_LOGE(TAG, "sendEvent: conversation.item.create requiere call_id y no hay uno disponible");
                cJSON_Delete(item);
                cJSON_Delete(root);
                return -1;
            }

            cJSON_AddItemToObject(root, "item", item);
        }
    }
    else if (strcmp(type, "system.message.create") == 0)
    {
        // Reemplazar el type original en root
        cJSON_DeleteItemFromObject(root, "type");
        // El tipo real que enviamos a OpenAI sigue siendo "conversation.item.create"
        cJSON_AddStringToObject(root, "type", "conversation.item.create");

        // Pero el 'item' se construye como un mensaje de sistema (según la doc)
        cJSON *item = cJSON_CreateObject();
        if (item)
        {
            cJSON_AddStringToObject(item, "type", "message"); // Tipo "message"
            cJSON_AddStringToObject(item, "role", "system");  // Rol "system"

            // Crear el objeto de contenido
            cJSON *content_array = cJSON_CreateArray();
            cJSON *content_item = cJSON_CreateObject();
            cJSON_AddStringToObject(content_item, "type", "input_text");
            cJSON_AddStringToObject(content_item, "text", text); // El 'text' es el mensaje
            cJSON_AddItemToArray(content_array, content_item);

            cJSON_AddItemToObject(item, "content", content_array);

            cJSON_AddItemToObject(root, "item", item);
        }
    }
    // Convertir a JSON sin formato (más eficiente)
    char *json_string = cJSON_PrintUnformatted(root);
    int ret = -1;
    if (json_string)
    {
        ret = webrtc_send_json(json_string);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to send WebRTC data, error: %d", ret);
        }
        free(json_string);
    }

    cJSON_Delete(root);
    return ret == ESP_OK ? 0 : ret;
}

/**
 * @brief Injects the physical-arrival context once the Realtime session is ready.
 */
int webrtc_inject_arrival_context(void)
{
    if (!g_realtime_session_ready)
    {
        ESP_LOGW(TAG, "Arrival context skipped; realtime session is not ready");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    cJSON *content_array = cJSON_CreateArray();
    cJSON *content_item = cJSON_CreateObject();
    if (!root || !item || !content_array || !content_item)
    {
        cJSON_Delete(root);
        cJSON_Delete(item);
        cJSON_Delete(content_array);
        cJSON_Delete(content_item);
        ESP_LOGE(TAG, "Arrival context: no memory to build user message");
        return -1;
    }

    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddItemToObject(root, "item", item);
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    cJSON_AddItemToObject(item, "content", content_array);
    cJSON_AddItemToArray(content_array, content_item);
    cJSON_AddStringToObject(content_item, "type", "input_text");
    const char *arrival_context_text =
        (g_webrtc_session_mode == WEBRTC_SESSION_MODE_VIGILANTE)
            ? VIGILANTE_ARRIVAL_PROMPT
            : SIMI_ARRIVAL_PROMPT;
    cJSON_AddStringToObject(content_item, "text", arrival_context_text);

    char *json_string = cJSON_PrintUnformatted(root);
    int ret = -1;
    if (json_string)
    {
        ret = webrtc_send_json(json_string);
        free(json_string);
    }
    else
    {
        ESP_LOGE(TAG, "Arrival context: failed to serialize user message");
    }
    cJSON_Delete(root);

    if (ret != 0)
    {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    webrtc_mark_activity();

    if (webrtc_realtime_is_busy())
    {
        ESP_LOGI(TAG, "Arrival context injected; response.create suppressed because Realtime is already responding");
        return 0;
    }

    if (esp_claw_is_fs_corrupted()) {
        sendEvent("system.message.create",
            "SYSTEM ALERT: The on-device filesystem failed to mount and the database is offline. "
            "Please inform the Architect that they need to connect the device to the PC and "
            "manually format the LittleFS partition using esptool.");
    }

    return sendEvent("response.create", NULL);
}

// --- AUTOMATION HANDLER TASK (Phase 3 Fix) ---
typedef struct {
    char call_id[128];
    char function_name[64];
    char *args_json;
} automation_task_ctx_t;

static void automation_handler_task(void *arg)
{
    automation_task_ctx_t *ctx = (automation_task_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    if (!esp_claw_is_automation_ready()) {
        ESP_LOGW(TAG, "Automation offline task running for function %s", ctx->function_name);
        const char* error_payload = "{\"error\": \"The automation system is currently offline or unavailable in this firmware version. Do not attempt further automation commands.\"}";
        send_function_output(ctx->call_id, error_payload);
    } else {
        ESP_LOGI(TAG, "Automation native task running for function %s", ctx->function_name);
        
        cJSON *args_root = NULL;
        if (ctx->args_json && strlen(ctx->args_json) > 0) {
            args_root = cJSON_Parse(ctx->args_json);
        }
        
        if (strcmp(ctx->function_name, "create_automation_rule") == 0) {
            cJSON *trigger_item = cJSON_GetObjectItemCaseSensitive(args_root, "trigger");
            cJSON *conditions_array = cJSON_GetObjectItemCaseSensitive(args_root, "conditions");
            cJSON *actions_array = cJSON_GetObjectItemCaseSensitive(args_root, "actions");
            
            if (cJSON_IsString(trigger_item) && cJSON_IsArray(conditions_array) && cJSON_IsArray(actions_array)) {
                esp_claw_rule_t *new_rule = heap_caps_malloc(sizeof(esp_claw_rule_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!new_rule) new_rule = malloc(sizeof(esp_claw_rule_t));
                
                if (new_rule) {
                    memset(new_rule, 0, sizeof(esp_claw_rule_t));
                    strlcpy(new_rule->call_id, ctx->call_id, sizeof(new_rule->call_id));
                    strlcpy(new_rule->trigger, trigger_item->valuestring, sizeof(new_rule->trigger));
                    
                    int num_conditions = cJSON_GetArraySize(conditions_array);
                    if (num_conditions > MAX_CONDITIONS) num_conditions = MAX_CONDITIONS;
                    new_rule->num_conditions = num_conditions;
                    
                    for (int i = 0; i < num_conditions; i++) {
                        cJSON *cond = cJSON_GetArrayItem(conditions_array, i);
                        cJSON *sensor = cJSON_GetObjectItemCaseSensitive(cond, "sensor");
                        cJSON *op = cJSON_GetObjectItemCaseSensitive(cond, "op");
                        cJSON *val = cJSON_GetObjectItemCaseSensitive(cond, "val");
                        
                        if (cJSON_IsString(sensor)) {
                            strlcpy(new_rule->conditions[i].sensor, sensor->valuestring, sizeof(new_rule->conditions[i].sensor));
                        }
                        if (cJSON_IsString(op)) {
                            strlcpy(new_rule->conditions[i].op, op->valuestring, sizeof(new_rule->conditions[i].op));
                        }
                        
                        if (cJSON_IsNumber(val)) {
                            new_rule->conditions[i].val_type = VAL_TYPE_NUMBER;
                            new_rule->conditions[i].f_val = (float)val->valuedouble;
                        } else if (cJSON_IsBool(val)) {
                            new_rule->conditions[i].val_type = VAL_TYPE_BOOL;
                            new_rule->conditions[i].b_val = (bool)cJSON_IsTrue(val);
                        } else if (cJSON_IsString(val)) {
                            new_rule->conditions[i].val_type = VAL_TYPE_STRING;
                            strlcpy(new_rule->conditions[i].s_val, val->valuestring, sizeof(new_rule->conditions[i].s_val));
                        }
                    }
                    
                    int num_actions = cJSON_GetArraySize(actions_array);
                    if (num_actions > MAX_ACTIONS) num_actions = MAX_ACTIONS;
                    new_rule->num_actions = num_actions;
                    
                    for (int i = 0; i < num_actions; i++) {
                        cJSON *action = cJSON_GetArrayItem(actions_array, i);
                        if (cJSON_IsString(action)) {
                            strlcpy(new_rule->actions[i].target, action->valuestring, sizeof(new_rule->actions[i].target));
                            new_rule->actions[i].target[255] = '\0';
                        }
                    }
                    
                    if (esp_claw_send_rule(new_rule) != ESP_OK) {
                        ESP_LOGW(TAG, "Rule queue full, dropping rule");
                        free(new_rule);
                        send_function_output(ctx->call_id, "{\"error\": \"Lua system busy\"}");
                    } else {
                        ESP_LOGI(TAG, "Rule successfully queued for async execution");
                    }
                } else {
                    send_function_output(ctx->call_id, "Error: out of memory.");
                }
            } else {
                send_function_output(ctx->call_id, "Error: missing or invalid trigger, conditions, or actions.");
            }
        } else if (strcmp(ctx->function_name, "list_automation_rules") == 0) {
            esp_claw_rule_t *list_req = heap_caps_malloc(sizeof(esp_claw_rule_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!list_req) list_req = malloc(sizeof(esp_claw_rule_t));
            if (list_req) {
                memset(list_req, 0, sizeof(esp_claw_rule_t));
                strlcpy(list_req->call_id, ctx->call_id, sizeof(list_req->call_id));
                strlcpy(list_req->trigger, "SYS_CMD:LIST", sizeof(list_req->trigger));
                if (esp_claw_send_rule(list_req) != ESP_OK) {
                    free(list_req);
                    send_function_output(ctx->call_id, "{\"error\": \"Lua system busy\"}");
                }
            } else {
                send_function_output(ctx->call_id, "{\"error\": \"out of memory\"}");
            }
        } else if (strcmp(ctx->function_name, "delete_automation_rule") == 0) {
            cJSON *trigger_item = cJSON_GetObjectItemCaseSensitive(args_root, "trigger");
            if (cJSON_IsString(trigger_item)) {
                esp_claw_rule_t *del_req = heap_caps_malloc(sizeof(esp_claw_rule_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!del_req) del_req = malloc(sizeof(esp_claw_rule_t));
                if (del_req) {
                    memset(del_req, 0, sizeof(esp_claw_rule_t));
                    strlcpy(del_req->call_id, ctx->call_id, sizeof(del_req->call_id));
                    strlcpy(del_req->trigger, "SYS_CMD:DELETE", sizeof(del_req->trigger));
                    del_req->num_actions = 1;
                    strlcpy(del_req->actions[0].target, trigger_item->valuestring, sizeof(del_req->actions[0].target));
                    if (esp_claw_send_rule(del_req) != ESP_OK) {
                        free(del_req);
                        send_function_output(ctx->call_id, "{\"error\": \"Lua system busy\"}");
                    }
                } else {
                    send_function_output(ctx->call_id, "{\"error\": \"out of memory\"}");
                }
            } else {
                send_function_output(ctx->call_id, "{\"error\": \"Missing or invalid trigger parameter.\"}");
            }
        } else if (strcmp(ctx->function_name, "execute_automation_trigger") == 0) {
            cJSON *trigger_item = cJSON_GetObjectItemCaseSensitive(args_root, "trigger");
            if (cJSON_IsString(trigger_item)) {
                esp_claw_rule_t *dummy_rule = heap_caps_malloc(sizeof(esp_claw_rule_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!dummy_rule) dummy_rule = malloc(sizeof(esp_claw_rule_t));
                
                if (dummy_rule) {
                    memset(dummy_rule, 0, sizeof(esp_claw_rule_t));
                    strlcpy(dummy_rule->call_id, ctx->call_id, sizeof(dummy_rule->call_id));
                    strlcpy(dummy_rule->trigger, "SYS_CMD:EXECUTE", sizeof(dummy_rule->trigger));
                    dummy_rule->num_actions = 1;
                    strlcpy(dummy_rule->actions[0].target, trigger_item->valuestring, sizeof(dummy_rule->actions[0].target));
                    dummy_rule->actions[0].target[sizeof(dummy_rule->actions[0].target)-1] = '\0';
                    
                    if (esp_claw_send_rule(dummy_rule) != ESP_OK) {
                        free(dummy_rule);
                        send_function_output(ctx->call_id, "{\"error\": \"Lua system busy\"}");
                    }
                } else {
                    send_function_output(ctx->call_id, "{\"error\": \"out of memory\"}");
                }
            } else {
                send_function_output(ctx->call_id, "{\"error\": \"Missing or invalid trigger parameter.\"}");
            }
        }

        
        if (args_root) cJSON_Delete(args_root);
    }
    
    if (ctx->args_json) free(ctx->args_json);
    free(ctx);
    vTaskDelete(NULL);
}

static void start_automation_task(const char* call_id, const char* function_name, const char* args_json)
{
    if (!call_id) return;
    automation_task_ctx_t *ctx = malloc(sizeof(automation_task_ctx_t));
    if (!ctx) return;
    
    strlcpy(ctx->call_id, call_id, sizeof(ctx->call_id));
    if (function_name) {
        strlcpy(ctx->function_name, function_name, sizeof(ctx->function_name));
    } else {
        ctx->function_name[0] = '\0';
    }
    
    if (args_json) {
        ctx->args_json = strdup(args_json);
    } else {
        ctx->args_json = NULL;
    }

    // Task Stack: 6144 bytes for native JSON parsing without IDLE starvation
    if (xTaskCreatePinnedToCore(automation_handler_task, "auto_handler", 6144, ctx, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create automation_handler_task");
        if (ctx->args_json) free(ctx->args_json);
        free(ctx);
    }
}
// ---------------------------------------------

/**
 * @brief Processes JSON data received via WebRTC.
 */
static int process_json(const char *json_data, int json_size)
{
    if (!json_data || json_size <= 0)
    {
        ESP_LOGE(TAG, "Error: json_data is NULL or empty");
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json_data, json_size);
    if (!root)
    {
        ESP_LOGE(TAG, "Error parsing JSON data");
        return -1;
    }

    // Capturar call_id si existe (y guardarlo para usarlo en la respuesta function_call_output)
    const char *call_id = NULL;
    const cJSON *call_id_item = cJSON_GetObjectItemCaseSensitive(root, "call_id");
    if (cJSON_IsString(call_id_item) && call_id_item->valuestring)
    {
        ESP_LOGI(TAG, "process_json: Capturado call_id: %s", call_id_item->valuestring);
        call_id = call_id_item->valuestring;
        set_call_id(call_id_item->valuestring);
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(root, "arguments");

    if (!cJSON_IsString(name) || !cJSON_IsString(arguments))
    {
        ESP_LOGW(TAG, "Invalid JSON: missing 'name' or 'arguments'");
        cJSON_Delete(root);
        return -1;
    }

    ESP_LOGI(TAG, "JSON class name: %s", name->valuestring);

    cJSON *args_root = cJSON_Parse(arguments->valuestring);
    if (!args_root)
    {
        ESP_LOGW(TAG, "Error parsing 'arguments' string as JSON");
        cJSON_Delete(root);
        return -1;
    }

    bool class_found = false;

    if (!class_found)
    {
        for (class_t *iter = classes; iter; iter = iter->next)
        {
            if (strcmp(iter->name, name->valuestring) != 0)
            {
                continue; // No es esta clase, sigue buscando
            }
    
            class_found = true;
            ESP_LOGI(TAG, "Procesando función: %s", iter->name);
    
            // --- MANEJO DE FUNCIONES SIN PARÁMETROS ESPECÍFICOS ---
            if (strcmp(iter->name, "enter_config_mode") == 0)
            {
                ESP_LOGW(TAG, "Llamada a función detectada! Activando modo de configuración...");
                start_config_mode_task();
                break; // Procesada
            }
            else if (strcmp(iter->name, "delete_api_key") == 0)
            {
                ESP_LOGI(TAG, "Llamada a función detectada! Borrando API Key...");
                start_delete_api_key_task();
                break; // Procesada
            }
            else if (strcmp(iter->name, "delete_credentials") == 0)
            { // <-- NUEVA FUNCIÓN
                ESP_LOGW(TAG, "Llamada a función detectada! Borrando TODAS las credenciales WiFi...");
                start_delete_credentials_task(); // <-- LLAMAR AL NUEVO LANZADOR
                break;                           // Procesada
            }
            else if (strcmp(iter->name, "activate_mute") == 0)
            { // <-- NEW FUNCTION
                ESP_LOGI(TAG, "Function call detected! Activating microphone mute...");
                start_activate_mute_task(); // <-- CALL NEW LAUNCHER
                break;                      // Processed
            }
            else if (strcmp(iter->name, "change_simi_outfit") == 0)
            {
                cJSON *outfit_item = cJSON_GetObjectItemCaseSensitive(args_root, "outfit_id");
                if (cJSON_IsString(outfit_item) && outfit_item->valuestring) {
                    if (strcmp(outfit_item->valuestring, "superhero") == 0) {
                        ui_simi_set_outfit(OUTFIT_CHAPULIN_RED);
                    } else if (strcmp(outfit_item->valuestring, "soccer") == 0) {
                        ui_simi_set_outfit(OUTFIT_SELECCION_GREEN);
                    } else if (strcmp(outfit_item->valuestring, "barca") == 0) {
                        ui_simi_set_outfit(OUTFIT_FC_BARCELONA);
                    } else {
                        ui_simi_set_outfit(OUTFIT_DOCTOR_WHITE);
                    }
                    send_function_output(call_id, "Outfit changed successfully.");
                } else {
                    send_function_output(call_id, "Error: Missing or invalid outfit_id.");
                }
                break;
            }
            // --- MANEJO DE FUNCIONES CON PARÁMETROS ---
            // Si llegamos aquí, la función SÍ espera parámetros.
    
            else if (strcmp(iter->name, "lookup_product_info") == 0)
            {
                cJSON *query_item = cJSON_GetObjectItemCaseSensitive(args_root, "query");
                if (cJSON_IsString(query_item) && query_item->valuestring && strlen(query_item->valuestring) > 0)
                {
                    ESP_LOGI(TAG, "Function call detected! Performing product lookup...");
                    start_lookup_product_task(query_item->valuestring, call_id);
                }
                else
                {
                    ESP_LOGW(TAG, "Invalid 'query' argument for lookup_product_info");
                    sendEvent("conversation.item.create", "Missing or invalid 'query' argument for lookup_product_info.");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sendEvent("response.create", "Lo siento, no pude realizar la consulta porque el nombre del producto es inválido.");
                    send_function_output(call_id, "Missing or invalid 'query' argument.");
                }
                break;
            }
            else if (strcmp(iter->name, "web_search") == 0)
            {
                cJSON *request_item = cJSON_GetObjectItemCaseSensitive(args_root, "request");
                if (cJSON_IsString(request_item) && request_item->valuestring && strlen(request_item->valuestring) > 0)
                {
                    ESP_LOGI(TAG, "Llamada a función detectada! Realizando búsqueda web...");
                    start_web_search_task("Lorenzo", request_item->valuestring, call_id, webrtc);
                }
                else
                {
                    ESP_LOGW(TAG, "Argumento 'request' inválido para web_search");
                    sendEvent("conversation.item.create", "Missing or invalid 'request' argument for web_search.");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sendEvent("response.create", "Lo siento, no pude realizar la búsqueda web porque el dato proporcionado es inválido.");
                }
                break; // Procesada (o falló el argumento)
            }
            else if (strcmp(iter->name, "control_display") == 0)
            {
                cJSON *state_item = cJSON_GetObjectItemCaseSensitive(args_root, "state");
                if (cJSON_IsString(state_item) && state_item->valuestring &&
                    (strcmp(state_item->valuestring, "on") == 0 || strcmp(state_item->valuestring, "off") == 0))
                {
                    ESP_LOGI(TAG, "Function call detected! Control display: '%s'", state_item->valuestring);
                    start_control_display_task(state_item->valuestring);
                }
                else
                {
                    ESP_LOGW(TAG, "Argumento 'state' inválido o faltante para control_display. Debe ser 'on' o 'off'.");
                    // --- TEXTO SIMPLE ---
                    sendEvent("conversation.item.create", "Missing or invalid 'state' argument. Must be 'on' or 'off'.");
                }
                break; // Procesada (o falló el argumento)
            }
            else if (strcmp(iter->name, "execute_automation_trigger") == 0)
            {
                start_automation_task(call_id, iter->name, arguments->valuestring);
                break; // Procesada
            }
            else
            {
                // Si encontramos una clase pero no coincide con ninguna de las anteriores
                ESP_LOGW(TAG, "Clase '%s' encontrada pero sin lógica de manejo implementada.", iter->name);
                break; // Salimos porque ya encontramos la clase
            }
    
        } // Fin del for loop
    }


    if (!class_found)
    {
        if (strcmp(name->valuestring, "list_automation_rules") == 0 ||
            strcmp(name->valuestring, "create_automation_rule") == 0 ||
            strcmp(name->valuestring, "delete_automation_rule") == 0)
        {
            start_automation_task(call_id, name->valuestring, arguments->valuestring);
            class_found = true;
        }
        else
        {
            ESP_LOGW(TAG, "No matching class handler found for name: %s", name->valuestring);
        }
    }

    cJSON_Delete(args_root);
    cJSON_Delete(root);
    return class_found ? 0 : -1;
}

static int webrtc_data_handler(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    if (!data || size <= 0)
    {
        ESP_LOGE(TAG, "Received invalid WebRTC data");
        return -1;
    }

    uint32_t generation = (uint32_t)(uintptr_t)ctx;
    if (!webrtc_generation_is_current(generation))
    {
        return 0;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)data, size);
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON data");
        return -1;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item) || !type_item->valuestring)
    {
        ESP_LOGW(TAG, "Event type not found or invalid");
        cJSON_Delete(root);
        return -1;
    }

    const char *event_type = type_item->valuestring;
    if (strcmp(event_type, "response.created") != 0)
    {
        track_data_channel_event(event_type, size);
    }

    if (strcmp(event_type, "error") == 0)
    {
        log_realtime_error(root);
        log_data_channel_snapshot("server-error");
        if (realtime_error_reports_active_response(root))
        {
            ESP_LOGW(TAG, "Realtime reports an active response; preserving response busy state");
            g_response_in_progress = true;
            webrtc_mark_activity();
        }
        else
        {
            g_response_in_progress = false;
            simi_session_set_state(SIMI_STATE_SAD, "server_error");
        }
    }
    else if (strcmp(event_type, "session.updated") == 0)
    {
        const bool first_session_ready = !g_realtime_session_ready;
        ESP_LOGI(TAG, "Realtime session updated; respuestas automaticas por VAD habilitadas");
        g_realtime_session_ready = true;
        webrtc_mark_activity();
        if (first_session_ready)
        {
            orchestrator_post_event(ORCH_EVENT_WEBRTC_CONNECTED);
            if (!orchestrator_get_mute_state()) {
                schedule_simi_session_visual(g_webrtc_session_mode == WEBRTC_SESSION_MODE_VIGILANTE
                                                 ? SIMI_STATE_ALERT
                                                 : SIMI_STATE_LISTENING,
                                             "session_updated");
            }
        }
        else
        {
            ESP_LOGD(TAG, "Duplicate session.updated ignored for orchestrator/Simi scheduling");
        }
    }
    else if (strcmp(event_type, "input_audio_buffer.speech_started") == 0)
    {
        g_input_speech_active = true;
        g_last_input_speech_ms = app_millis();
        webrtc_mark_activity();

        if (g_response_in_progress)
        {
            webrtc_send_json("{\"type\":\"response.cancel\"}");
            g_response_in_progress = false;
        }
    }
    else if (strcmp(event_type, "input_audio_buffer.speech_stopped") == 0)
    {
        g_input_speech_active = false;
        g_last_input_speech_ms = app_millis();
        webrtc_mark_activity();
    }
    else if (strcmp(event_type, "input_audio_buffer.committed") == 0)
    {
        webrtc_mark_activity();
    }
    else if (strcmp(event_type, "response.created") == 0)
    {
        g_response_in_progress = true;
        webrtc_mark_activity();
        reset_data_channel_response_stats();
        track_data_channel_event(event_type, size);
        if (g_webrtc_session_mode != WEBRTC_SESSION_MODE_VIGILANTE)
        {
            simi_session_set_state(SIMI_STATE_THINKING, "response_created");
        }
    }
    else if (strcmp(event_type, "response.done") == 0)
    {
        log_response_done(root);
        log_data_channel_snapshot("response-done");
        g_response_in_progress = false;
        webrtc_mark_activity();
        schedule_post_response_capture_recovery();
        if (!g_output_audio_active)
        {
            simi_session_notify_speaking(false);
            if (orchestrator_get_mute_state()) {
                simi_session_set_state(SIMI_STATE_MUTED, "response_done_muted");
            } else {
                simi_session_set_state(g_webrtc_session_mode == WEBRTC_SESSION_MODE_VIGILANTE
                                           ? SIMI_STATE_ALERT
                                           : SIMI_STATE_LISTENING,
                                       "response_done");
            }
        }
    }
    else if (strcmp(event_type, "response.audio.done") == 0 ||
             strcmp(event_type, "response.output_audio.done") == 0)
    {
        webrtc_mark_activity();
    }
    else if (strcmp(event_type, "output_audio_buffer.started") == 0)
    {
        g_output_audio_active = true;
        webrtc_mark_activity();
        simi_session_notify_speaking(true);
    }
    else if (strcmp(event_type, "output_audio_buffer.stopped") == 0 ||
             strcmp(event_type, "output_audio_buffer.cleared") == 0)
    {
        g_output_audio_active = false;
        g_last_output_audio_stopped_ms = app_millis();
        webrtc_mark_activity();
        simi_session_notify_speaking(false);
        if (orchestrator_get_mute_state()) {
            simi_session_set_state(SIMI_STATE_MUTED, "output_audio_done_muted");
        } else {
            simi_session_set_state(g_webrtc_session_mode == WEBRTC_SESSION_MODE_VIGILANTE
                                       ? SIMI_STATE_ALERT
                                       : SIMI_STATE_LISTENING,
                                   "output_audio_done");
        }
    }
    else if (strcmp(event_type, "conversation.item.input_audio_transcription.completed") == 0 ||
             strcmp(event_type, "response.audio_transcript.done") == 0 ||
             strcmp(event_type, "response.output_audio_transcript.done") == 0)
    {
        const char *transcript = find_transcript_text(root);
        const char *label = strcmp(event_type, "conversation.item.input_audio_transcription.completed") == 0
                                ? "Input Transcript"
                                : "Output Transcript";
        if (transcript)
        {
            ESP_LOGI(TAG, "%s: %s", label, transcript);
        }
        else
        {
            ESP_LOGW(TAG, "%s event without transcript payload (%s)", label, event_type);
        }
    }
    else if (strcmp(event_type, "conversation.item.input_audio_transcription.failed") == 0)
    {
        ESP_LOGW(TAG, "Input transcription failed");
        log_realtime_error(root);
    }
    else if (strcmp(event_type, "response.function_call_arguments.done") == 0)
    {
        if (g_webrtc_session_mode != WEBRTC_SESSION_MODE_VIGILANTE)
        {
            simi_session_set_state(SIMI_STATE_THINKING, "function_call_arguments_done");
        }
        process_json((const char *)data, size);
    }
    else if (strcmp(event_type, "response.output_item.added") == 0 ||
             strcmp(event_type, "response.output_item.done") == 0)
    {
        log_response_output_item(root, event_type);
    }
    else if (strncmp(event_type, "response.", strlen("response.")) == 0 ||
             strncmp(event_type, "conversation.item.", strlen("conversation.item.")) == 0)
    {
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        if (strstr(event_type, ".delta") == NULL)
        {
            ESP_LOGI(TAG, "Realtime event: %s", event_type);
        }
#endif
    }
    else if (strstr(event_type, ".delta") == NULL)
    {
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGI(TAG, "Realtime event: %s", event_type);
#endif
    }

    cJSON_Delete(root);
    return 0;
}

static int webrtc_event_handler(esp_webrtc_event_t *event, void *ctx)
{
    if (!event)
    {
        ESP_LOGE(TAG, "Error: event pointer is NULL");
        return -1;
    }

    uint32_t generation = (uint32_t)(uintptr_t)ctx;
    if (!webrtc_generation_is_current(generation))
    {
        return 0;
    }

    switch (event->type)
    {
    case ESP_WEBRTC_EVENT_DATA_CHANNEL_CONNECTED:
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGI(TAG, "✅ Data Channel Connected - Sending session update...");
#endif
        ESP_LOGI(TAG, "Data Channel Connected - sending session.update");
        g_realtime_session_ready = false;
        xEventGroupSetBits(app_startup_event_group, WEBRTC_CONNECTED_BIT);
        xEventGroupClearBits(app_startup_event_group, WEBRTC_API_ERROR_BIT | WEBRTC_DISCONNECTED_BIT);
        schedule_session_update();
        break;

    case ESP_WEBRTC_EVENT_DATA_CHANNEL_DISCONNECTED:
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGW(TAG, "⚠️ Data Channel Disconnected");
#endif
        ESP_LOGW(TAG, "Data Channel Disconnected");
        log_data_channel_snapshot("data-channel-disconnected");
        reset_realtime_interaction_state();
        xEventGroupClearBits(app_startup_event_group, WEBRTC_CONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WEBRTC_DISCONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
        display_simi_session_state(SIMI_STATE_SAD, "data_channel_disconnected");
        break;

    case ESP_WEBRTC_EVENT_DISCONNECTED:
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGW(TAG, "⚠️ WebRTC Disconnected");
#endif
        ESP_LOGW(TAG, "WebRTC Disconnected");
        log_data_channel_snapshot("webrtc-disconnected");
        reset_realtime_interaction_state();
        xEventGroupClearBits(app_startup_event_group, WEBRTC_CONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WEBRTC_DISCONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
        display_simi_session_state(SIMI_STATE_SAD, "webrtc_disconnected");
        break;

    case ESP_WEBRTC_EVENT_CONNECTED:
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGI(TAG, "✅ WebRTC Connected (peer level)");
        // No setear WEBRTC_CONNECTED_BIT aquí, esperar al data channel
        break;

#endif
        ESP_LOGI(TAG, "WebRTC Connected (peer level)");
        break;

    case ESP_WEBRTC_EVENT_CONNECT_FAILED:
#if ENABLE_REALTIME_EVENT_DEBUG_LOGS
        ESP_LOGE(TAG, "❌ WebRTC Connection Failed");
#endif
        ESP_LOGE(TAG, "WebRTC Connection Failed");
        log_data_channel_snapshot("connect-failed");
        reset_realtime_interaction_state();
        xEventGroupClearBits(app_startup_event_group, WEBRTC_CONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WEBRTC_DISCONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
        display_simi_session_state(SIMI_STATE_SAD, "webrtc_connect_failed");
        break;

    default:
        ESP_LOGW(TAG, "Unhandled WebRTC Event: %d", event->type);
        break;
    }

    return 0;
}

int start_webrtc(webrtc_session_mode_t mode)
{

    // --- INICIO DE LA NUEVA LÓGICA ---
    // 1. Preguntar al "llavero" por la llave a usar.
    const char *api_key = config_manager_get_current_api_key();

    // 2. Validar la llave que nos dieron.
    if (!api_key || strlen(api_key) == 0)
    {
        ESP_LOGE(TAG, "start_webrtc: El Config Manager no proporcionó una API Key válida.");
        // Levantar la bandera de error para que el Orquestador se entere.
        xEventGroupSetBits(app_startup_event_group, WEBRTC_API_ERROR_BIT);
        return -1;
    }
    ESP_LOGI(TAG, "Iniciando WebRTC con la llave de fuente: %d", config_manager_get_current_source());
    if (ensure_webrtc_mutex() != ESP_OK)
    {
        ESP_LOGE(TAG, "start_webrtc: no se pudo crear mutex de lifecycle");
        return -1;
    }
    // --- FIN DE LA NUEVA LÓGICA ---

    // Inicializar mutex para call_id si aún no existe
    if (g_call_id_mutex == NULL)
    {
        g_call_id_mutex = xSemaphoreCreateMutex();
        if (g_call_id_mutex == NULL)
        {
            ESP_LOGW(TAG, "start_webrtc: fallo al crear mutex para call_id");
        }
    }

    // Construir clases si no se han construido ya
    build_classes();

    if (network_is_connected() == false)
    {
        ESP_LOGE(TAG, "Wifi not connected yet");
        return -1;
    }

    esp_webrtc_handle_t old_handle = NULL;
    uint32_t generation = 0;
    if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        old_handle = webrtc;
        webrtc = NULL;
        g_webrtc_generation++;
        generation = g_webrtc_generation;
        g_session_update_task = NULL;
        g_session_update_generation = 0;
        reset_realtime_interaction_state();
        g_webrtc_session_mode = mode;
        g_last_realtime_activity_ms = app_millis();
        xSemaphoreGive(g_webrtc_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "start_webrtc: no se pudo tomar mutex de lifecycle");
        return -1;
    }

    if (old_handle != NULL)
    {
        esp_webrtc_close(old_handle);
    }

    xEventGroupClearBits(app_startup_event_group, WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT);

    esp_peer_default_cfg_t peer_cfg = {
        .agent_recv_timeout = 500,
    };

    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            .audio_info = {
#ifdef WEBRTC_SUPPORT_OPUS
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel = 1,
#else
                .codec = ESP_PEER_AUDIO_CODEC_G711A,
#endif
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .enable_data_channel = DATA_CHANNEL_ENABLED,
            .no_auto_reconnect = true,
            .on_custom_data = webrtc_data_handler,
            .ctx = (void *)(uintptr_t)generation,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .signaling_cfg.extra_cfg = (void *)api_key,
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_signaling_get_openai_signaling(),
    };

    esp_webrtc_handle_t new_handle = NULL;
    int ret = esp_webrtc_open(&cfg, &new_handle);
    if (ret != 0)
    {
        g_webrtc_session_mode = WEBRTC_SESSION_MODE_FRIENDLY;
        ESP_LOGE(TAG, "❌ Fail to open webrtc (error code: %d)", ret);
        return ret;
    }

    // Set media provider
    esp_webrtc_media_provider_t media_provider = {};
    media_sys_get_provider(&media_provider);
    esp_webrtc_set_media_provider(new_handle, &media_provider);

    // Set event handler
    esp_webrtc_set_event_handler(new_handle, webrtc_event_handler, (void *)(uintptr_t)generation);

    if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        webrtc = new_handle;
        xSemaphoreGive(g_webrtc_mutex);
    }
    else
    {
        esp_webrtc_close(new_handle);
        return -1;
    }

    // Start webrtc
    ret = esp_webrtc_start(new_handle);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "❌ Fail to start webrtc (error code: %d)", ret);
        if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            if (webrtc == new_handle)
            {
                webrtc = NULL;
                g_webrtc_generation++;
                g_session_update_task = NULL;
                g_session_update_generation = 0;
                g_webrtc_session_mode = WEBRTC_SESSION_MODE_FRIENDLY;
                reset_realtime_interaction_state();
            }
            xSemaphoreGive(g_webrtc_mutex);
        }
        esp_webrtc_close(new_handle);
    }
    else
    {
        ESP_LOGI(TAG, "✅ WebRTC iniciado correctamente");
    }

    return ret;
}

void query_webrtc(void)
{
#if ENABLE_WEBRTC_PERIODIC_QUERY_LOGS
    if (ensure_webrtc_mutex() == ESP_OK &&
        xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        esp_webrtc_handle_t handle = webrtc;
        if (handle)
        {
            esp_webrtc_query(handle);
        }
        xSemaphoreGive(g_webrtc_mutex);
    }
#endif
}

int stop_webrtc(void)
{
    esp_webrtc_handle_t handle = NULL;

    if (ensure_webrtc_mutex() != ESP_OK)
    {
        return -1;
    }

    if (xSemaphoreTake(g_webrtc_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        handle = webrtc;
        webrtc = NULL;
        g_webrtc_generation++;
        g_session_update_task = NULL;
        g_session_update_generation = 0;
        g_webrtc_session_mode = WEBRTC_SESSION_MODE_FRIENDLY;
        reset_realtime_interaction_state();
        xSemaphoreGive(g_webrtc_mutex);
    }
    else
    {
        return -1;
    }

    if (handle != NULL)
    {
        esp_webrtc_close(handle);
    }

    media_sys_teardown();

    return 0;
}
