#include "responses_client.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "ui.h"
#include "prompts.h"
#include "settings.h"
#include "webrtc.h"
#include "http_client.h"
#include "config_manager.h"
#include "esp_crt_bundle.h"

static const char *TAG = "RESPONSES_CLIENT";

typedef struct {
    char call_id[64];
    char query[256];
} http_msg_t;

static QueueHandle_t s_http_queue = NULL;

static void lookup_product_task(void *pvParameters)
{
    http_msg_t msg;
    for (;;)
    {
        if (xQueueReceive(s_http_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGI(TAG, "LOOKUP_PRODUCT_TASK: Query = '%s'", msg.query);
            ui_show_status_message("Consulting...", COLOR_WHITE_BGR565);
            char *post_data = NULL;

            // Construir el payload JSON de búsqueda estricta
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "query", msg.query);
            
            post_data = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (!post_data) {
                send_function_output(msg.call_id, "Failed to build JSON payload.");
                vTaskDelay(pdMS_TO_TICKS(200));
                sendEvent("response.create", "Hubo un error al preparar la búsqueda.");
                goto cleanup_msg;
            }

            const char *current_api_key = config_manager_get_cached_api_key();
            if (current_api_key == NULL || current_api_key[0] == '\0') {
                send_function_output(msg.call_id, "Missing API Key.");
                vTaskDelay(pdMS_TO_TICKS(200));
                sendEvent("response.create", "Lo siento, no tengo configurada la clave de API.");
                goto cleanup_msg;
            }

            // Configurar URL dinámica apuntando directamente al endpoint del Vector Store
            char search_url[256];
            snprintf(search_url, sizeof(search_url), "https://api.openai.com/v1/vector_stores/%s/search", VECTOR_STORE_ID);

            // Inicializar la estructura transaccional usando tu http_client.h heredado
            http_request_t request = {0};
            request.method = HTTP_METHOD_POST;
            request.url = search_url;
            request.payload = post_data;
            request.api_key = current_api_key;
            request.headers = "OpenAI-Beta: assistants=v2"; // Añadido para compatibilidad con Vector Store
            request.timeout_ms = 30000; // Timeout extendido para búsquedas vectoriales complejas
            
            http_response_t response = {0};
            esp_err_t err = http_client_request(&request, &response);
            
            if (err == ESP_OK && response.status_code == 200 && response.response) {
                cJSON *resp_json = cJSON_Parse(response.response);
                if (resp_json) {
                    cJSON *data_array = cJSON_GetObjectItem(resp_json, "data");
                    if (data_array && cJSON_IsArray(data_array)) {
                        int data_size = cJSON_GetArraySize(data_array);
                        if (data_size > 0) {
                            char *context_buffer = calloc(1, 16384); // Buffer de 16KB seguro en PSRAM para los chunks
                            if (context_buffer) {
                                for (int i = 0; i < data_size; i++) {
                                    cJSON *data_item = cJSON_GetArrayItem(data_array, i);
                                    cJSON *content_array = cJSON_GetObjectItem(data_item, "content");
                                    if (content_array && cJSON_IsArray(content_array)) {
                                        int content_size = cJSON_GetArraySize(content_array);
                                        for (int j = 0; j < content_size; j++) {
                                            cJSON *content_item = cJSON_GetArrayItem(content_array, j);
                                            cJSON *text_item = cJSON_GetObjectItem(content_item, "text");
                                            if (text_item && cJSON_IsString(text_item)) {
                                                strncat(context_buffer, text_item->valuestring, 16384 - strlen(context_buffer) - 1);
                                                strncat(context_buffer, "\n", 16384 - strlen(context_buffer) - 1);
                                            }
                                        }
                                    }
                                }
                                
                                // Validar si extrajimos texto real de los vectores
                                if (strlen(context_buffer) > 0) {
                                    send_function_output(msg.call_id, context_buffer);
                                    vTaskDelay(pdMS_TO_TICKS(200)); // Retardo esencial para asimilación del servidor
                                    sendEvent("response.create", ""); 
                                } else {
                                    send_function_output(msg.call_id, "No encontré información relevante en la base de datos.");
                                    vTaskDelay(pdMS_TO_TICKS(200));
                                    sendEvent("response.create", ""); 
                                }
                                free(context_buffer);
                            } else {
                                send_function_output(msg.call_id, "Memory allocation error during search extraction.");
                                vTaskDelay(pdMS_TO_TICKS(200));
                                sendEvent("response.create", "Lo siento, hubo un problema al procesar la respuesta.");
                            }
                        } else {
                            send_function_output(msg.call_id, "No encontré información relevante en la base de datos.");
                            vTaskDelay(pdMS_TO_TICKS(200));
                            sendEvent("response.create", ""); 
                        }
                    } else {
                        send_function_output(msg.call_id, "No encontré información relevante en la base de datos.");
                        vTaskDelay(pdMS_TO_TICKS(200));
                        sendEvent("response.create", ""); 
                    }
                    cJSON_Delete(resp_json);
                } else {
                    send_function_output(msg.call_id, "Failed to parse backend response.");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sendEvent("response.create", "Lo siento, hubo un problema al leer la respuesta de la base de datos.");
                }
            } else {
                ESP_LOGE(TAG, "HTTP POST request failed (err: %d) or returned error status code: %d", err, response.status_code);
                send_function_output(msg.call_id, "Backend HTTP request failed or timed out.");
                vTaskDelay(pdMS_TO_TICKS(200));
                sendEvent("response.create", "Lo siento, hubo un problema de red al consultar los productos. Inténtalo de nuevo más tarde.");
            }

            if (response.response) {
                free(response.response);
            }

        cleanup_msg:
            ui_clear_status_message();
            if (post_data) free(post_data);
        }
    }
}

void http_worker_init(void)
{
    if (s_http_queue != NULL) {
        return;
    }
    
    s_http_queue = xQueueCreate(3, sizeof(http_msg_t));
    if (s_http_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create http queue");
        return;
    }

    void *stack_buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    void *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!stack_buffer || !tcb) {
        ESP_LOGE(TAG, "Error allocating PSRAM or TCB for persistent http_worker_task");
        if (stack_buffer) heap_caps_free(stack_buffer);
        if (tcb) heap_caps_free(tcb);
        return;
    }
    
    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        lookup_product_task, 
        "http_worker", 
        8192, 
        NULL, 
        tskIDLE_PRIORITY + 1, 
        stack_buffer, 
        tcb, 
        APP_CPU_NUM
    );

    if (handle == NULL) {
        ESP_LOGE(TAG, "Error creating persistent http_worker_task");
    }
}

void start_lookup_product_task(const char *query, const char *call_id)
{
    if (s_http_queue == NULL) {
        send_function_output(call_id, "System failure: HTTP queue not initialized.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "El sistema no está listo para buscar.");
        return;
    }

    http_msg_t msg;
    memset(&msg, 0, sizeof(http_msg_t));
    if (call_id) strncpy(msg.call_id, call_id, sizeof(msg.call_id) - 1);
    if (query) strncpy(msg.query, query, sizeof(msg.query) - 1);

    if (xQueueSend(s_http_queue, &msg, 0) != pdPASS) {
        ESP_LOGE(TAG, "HTTP Queue is full, dropping request");
        send_function_output(call_id, "System busy, try again later.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "Estoy procesando otra búsqueda, intenta de nuevo en un momento.");
    }
}