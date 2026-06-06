#include "web_search.h"
#include "http_client.h"
#include <esp_log.h>
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "config_manager.h"

#define TAG "WEB_SEARCH"

char *web_search(const char *request_input)
{
    if (!request_input)
    {
        ESP_LOGE(TAG, "Invalid request input");
        return NULL;
    }

    // Crear el objeto JSON raíz para construir el payload
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create root JSON object");
        return NULL;
    }
    // Agregar el modelo
    cJSON_AddStringToObject(root, "model", "gpt-4o-mini-2024-07-18");
    cJSON_AddNumberToObject(root, "max_output_tokens", 512);

    // Crear el array 'tools'
    cJSON *tools_array = cJSON_CreateArray();
    if (!tools_array)
    {
        ESP_LOGE(TAG, "Failed to create tools array");
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *tool_obj = cJSON_CreateObject();
    if (!tool_obj)
    {
        ESP_LOGE(TAG, "Failed to create tool object");
        cJSON_Delete(tools_array);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(tool_obj, "type", "web_search_preview");

    // Si se proporciona user_location, agregar el objeto 'user_location'
    // if (user_location)
    // {
    //     cJSON *location_obj = cJSON_CreateObject();
    //     if (!location_obj)
    //     {
    //         ESP_LOGE(TAG, "Failed to create user_location object");
    //         cJSON_Delete(tool_obj);
    //         cJSON_Delete(tools_array);
    //         cJSON_Delete(root);
    //         return NULL;
    //     }
    //     if (user_location->type)
    //         cJSON_AddStringToObject(location_obj, "type", user_location->type);
    //     if (user_location->country)
    //         cJSON_AddStringToObject(location_obj, "country", user_location->country);
    //     if (user_location->city)
    //         cJSON_AddStringToObject(location_obj, "city", user_location->city);
    //     if (user_location->region)
    //         cJSON_AddStringToObject(location_obj, "region", user_location->region);
    //     cJSON_AddItemToObject(tool_obj, "user_location", location_obj);
    // }
    cJSON_AddItemToArray(tools_array, tool_obj);
    cJSON_AddItemToObject(root, "tools", tools_array);

    // Agregar el input del request
    cJSON_AddStringToObject(root, "input", request_input);

    // Convertir el JSON a cadena sin formato
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
    {
        ESP_LOGE(TAG, "Failed to generate payload string");
        return NULL;
    }
    const char *current_api_key = config_manager_get_current_api_key();
    if (!current_api_key)
    {
        ESP_LOGE(TAG, "No se pudo obtener una API Key válida desde el Config Manager.");
        free(payload);
        return NULL;
    }

    // Configurar la solicitud HTTP usando el módulo genérico
    http_request_t request = {0};
    request.url = "https://api.openai.com/v1/responses";
    request.method = HTTP_METHOD_POST;
    request.payload = payload;
    request.api_key = current_api_key;
    // No se requieren headers adicionales; el módulo se encarga de Content-Type y Authorization.
    request.timeout_ms = 15000;

    http_response_t http_resp = {0};
    esp_err_t err = http_client_request(&request, &http_resp);
    free(payload);
    if (err != ESP_OK || http_resp.response == NULL)
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return NULL;
    }

    ESP_LOGD(TAG, "Raw web search response: %s", http_resp.response);

    // Parsear la respuesta JSON: ahora la respuesta es un objeto que contiene la clave "output"
    cJSON *json_root = cJSON_Parse(http_resp.response);
    free(http_resp.response);
    if (!json_root || !cJSON_IsObject(json_root))
    {
        ESP_LOGE(TAG, "Failed to parse response JSON or response is not an object");
        if (json_root)
            cJSON_Delete(json_root);
        return NULL;
    }

    cJSON *resp_array = cJSON_GetObjectItem(json_root, "output");
    if (!cJSON_IsArray(resp_array))
    {
        ESP_LOGE(TAG, "Failed to parse response JSON: 'output' is not an array");
        cJSON_Delete(json_root);
        return NULL;
    }

    // Buscar el objeto con "type" == "message"
    cJSON *message_obj = NULL;
    int array_size = cJSON_GetArraySize(resp_array);
    for (int i = 0; i < array_size; i++)
    {
        cJSON *item = cJSON_GetArrayItem(resp_array, i);
        cJSON *type_item = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "message") == 0)
        {
            message_obj = item;
            break;
        }
    }
    if (!message_obj)
    {
        ESP_LOGE(TAG, "No message object found in response");
        cJSON_Delete(json_root);
        return NULL;
    }

    // Obtener el array "content" y extraer el primer objeto
    cJSON *content_array = cJSON_GetObjectItem(message_obj, "content");
    if (!content_array || !cJSON_IsArray(content_array))
    {
        ESP_LOGE(TAG, "Invalid or missing content array in message object");
        cJSON_Delete(json_root);
        return NULL;
    }
    cJSON *first_content = cJSON_GetArrayItem(content_array, 0);
    if (!first_content)
    {
        ESP_LOGE(TAG, "Content array is empty");
        cJSON_Delete(json_root);
        return NULL;
    }
    cJSON *text_item = cJSON_GetObjectItem(first_content, "text");
    if (!text_item || !cJSON_IsString(text_item))
    {
        ESP_LOGE(TAG, "No text found in first content item");
        cJSON_Delete(json_root);
        return NULL;
    }

    char *result = strdup(text_item->valuestring);
    cJSON_Delete(json_root);
    return result;
}
