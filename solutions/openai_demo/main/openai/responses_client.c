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

static const char *TAG = "RESPONSES_CLIENT";

static void lookup_product_task(void *pvParameters)
{
    lookup_task_args_t *args = (lookup_task_args_t *)pvParameters;
    
    // Extraer parámetros y liberar la estructura contenedora inmediatamente
    char *call_id = NULL;
    char *query = NULL;
    if (args->call_id) call_id = strdup(args->call_id);
    if (args->query) query = strdup(args->query);
    if (args->call_id) free(args->call_id);
    if (args->query) free(args->query);
    free(args);

    if (!call_id || !query) {
        if (call_id) free(call_id);
        if (query) free(query);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "LOOKUP_PRODUCT_TASK: Query = '%s'", query);
    ui_show_status_message("Consulting...", COLOR_WHITE_BGR565);

    // Construir el payload JSON de búsqueda estricta
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "query", query);
    
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!post_data) {
        send_function_output(call_id, "Failed to build JSON payload.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "Hubo un error al preparar la búsqueda.");
        goto cleanup;
    }

    const char *current_api_key = config_manager_get_current_api_key();
    if (!current_api_key) {
        send_function_output(call_id, "Missing API Key.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "Lo siento, no tengo configurada la clave de API.");
        if (post_data) free(post_data);
        free(call_id);
        free(query);
        vTaskDelete(NULL);
        return;
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
                            send_function_output(call_id, context_buffer);
                            vTaskDelay(pdMS_TO_TICKS(200)); // Retardo esencial para asimilación del servidor
                            sendEvent("response.create", ""); 
                        } else {
                            send_function_output(call_id, "No encontré información relevante en la base de datos.");
                            vTaskDelay(pdMS_TO_TICKS(200));
                            sendEvent("response.create", ""); 
                        }
                        free(context_buffer);
                    } else {
                        send_function_output(call_id, "Memory allocation error during search extraction.");
                        vTaskDelay(pdMS_TO_TICKS(200));
                        sendEvent("response.create", "Lo siento, hubo un problema al procesar la respuesta.");
                    }
                } else {
                    send_function_output(call_id, "No encontré información relevante en la base de datos.");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sendEvent("response.create", ""); 
                }
            } else {
                send_function_output(call_id, "No encontré información relevante en la base de datos.");
                vTaskDelay(pdMS_TO_TICKS(200));
                sendEvent("response.create", ""); 
            }
            cJSON_Delete(resp_json);
        } else {
            send_function_output(call_id, "Failed to parse backend response.");
            vTaskDelay(pdMS_TO_TICKS(200));
            sendEvent("response.create", "Lo siento, hubo un problema al leer la respuesta de la base de datos.");
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed (err: %d) or returned error status code: %d", err, response.status_code);
        send_function_output(call_id, "Backend HTTP request failed or timed out.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "Lo siento, hubo un problema de red al consultar los productos. Inténtalo de nuevo más tarde.");
    }

    if (response.response) {
        free(response.response);
    }

cleanup:
    ui_clear_status_message();
    if (post_data) free(post_data);
    free(call_id);
    free(query);
    vTaskDelete(NULL);
}

void start_lookup_product_task(const char *query, const char *call_id)
{
    lookup_task_args_t *args = calloc(1, sizeof(lookup_task_args_t));
    if (args)
    {
        args->call_id = strdup(call_id ? call_id : "");
        args->query = strdup(query ? query : "");
        
        if (xTaskCreatePinnedToCore(lookup_product_task, "lookup_product", 8192, args, tskIDLE_PRIORITY + 1, NULL, APP_CPU_NUM) != pdPASS)
        {
            ESP_LOGE(TAG, "Error creating lookup_product_task");
            free(args->call_id);
            free(args->query);
            free(args);
            
            send_function_output(call_id, "System failure: could not launch task.");
            vTaskDelay(pdMS_TO_TICKS(200));
            sendEvent("response.create", "Lo siento, falló el sistema al intentar buscar.");
        }
    }
    else
    {
        send_function_output(call_id, "System failure: memory allocation failed.");
        vTaskDelay(pdMS_TO_TICKS(200));
        sendEvent("response.create", "Lo siento, me quedé sin memoria para buscar.");
    }
}