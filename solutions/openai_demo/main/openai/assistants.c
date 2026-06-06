#include "assistants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include "settings.h"
#include "http_client.h"

#define TAG "OPENAI_ASSISTANT_API"

// Función que envuelve la llamada al módulo HTTP genérico
char *assistants_send_request(const char *api_key, const char *assistant_url, const char *payload)
{
    http_request_t request = {0};

    // Determinar el método: GET si no hay payload, POST si lo hay.
    request.method = (payload == NULL) ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    request.url = (char *)assistant_url;
    request.payload = (char *)payload;
    request.api_key = api_key;

    // Configurar encabezados: puedes combinar múltiples en una sola cadena
    // Nota: Asegúrate de construir la cadena correctamente o mejorar el módulo para múltiples encabezados.
    request.headers = "OpenAI-Beta: assistants=v2";
    request.timeout_ms = 30000;

    http_response_t response = {0};
    esp_err_t err = http_client_request(&request, &response);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Solicitud HTTP fallida: %s", esp_err_to_name(err));

        // --- INICIO DE CAMBIO PARA DIAGNÓSTICO ---
        // En lugar de devolver NULL, creamos una respuesta JSON de error.
        // Esto nos ayuda a ver si el problema es el NULL o si la tarea se congela antes.
        const char *err_name = esp_err_to_name(err);
        // Asignamos memoria para el mensaje de error JSON. +30 para el texto del formato.
        char *error_response = malloc(strlen(err_name) + 30);
        if (error_response)
        {
            sprintf(error_response, "{\"assistant_response\":\"ERROR: %s\"}", err_name);
        }
        return error_response; // Devolvemos la cadena de error. El llamador DEBE liberarla.
        // --- FIN DE CAMBIO PARA DIAGNÓSTICO ---
    }

    // La respuesta queda en response.response, la cual es memoria asignada dinámicamente.
    // Quien llame a esta función debe liberar esa memoria cuando ya no se necesite.
    return response.response;
}

// Función para manejar la comunicación con la API externa
char *assistants_assistantManager(Assistants *assistants, const char *user, const char *message)
{
    if (!assistants || !user || !message)
    {
        ESP_LOGE("AssistantManager", "Invalid parameters");
        return NULL;
    }

    const char *assistantId = assistants->santiagoId;
    if (!assistantId)
    {
        ESP_LOGE("AssistantManager", "Assistant ID is missing");
        return NULL;
    }

    char *response = NULL;

    // Buscar si ya hay un thread asignado al usuario
    char *threadId = NULL;
    for (int i = 0; i < 5; i++)
    {
        if (assistants->threads[i].user && strcmp(assistants->threads[i].user, user) == 0)
        {
            threadId = assistants->threads[i].threadId;
            break;
        }
    }

    if (threadId)
    {
        ESP_LOGI("AssistantManager", "Using existing thread: %s", threadId);
        // Agregar mensaje al hilo existente
        if (!createMessage(assistants, threadId, message))
        {
            ESP_LOGE("AssistantManager", "Failed to create message");
            return NULL;
        }
    }
    else
    {
        ESP_LOGI("AssistantManager", "Creating new thread for user: %s", user);
        // Crear un nuevo thread para el usuario
        char *newThreadId = createThread(assistants, message);
        if (!newThreadId)
        {
            ESP_LOGE("AssistantManager", "Failed to create thread");
            return NULL;
        }

        // Guardar el thread en la estructura Assistants
        for (int i = 0; i < 5; i++)
        {
            if (!assistants->threads[i].user) // Espacio vacío en la tabla
            {
                assistants->threads[i].user = strdup(user);
                assistants->threads[i].threadId = strdup(newThreadId);
                break;
            }
        }
        threadId = newThreadId;
    }

    // Ejecutar el asistente en el thread del usuario
    if (!runAssistant(assistants, threadId, assistantId))
    {
        return NULL; // Si falla, retornar NULL
    }

    // Obtener la respuesta desde los mensajes
    response = listMessages(assistants, threadId);
    if (!response)
    {
        ESP_LOGE("AssistantManager", "Failed to list messages");
        return NULL;
    }

    return response;
}

// Función para crear un thread
char *createThread(Assistants *assistants, const char *context)
{
    if (assistants == NULL || assistants->apiKey == NULL || context == NULL)
    {
        ESP_LOGE("HTTP", "Invalid parameters passed to createThread()");
        return NULL;
    }

    // Crear JSON con el contexto del usuario
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    if (root == NULL || messages == NULL || message == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON structure");
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(message);
        return NULL;
    }

    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", context);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_string == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON string");
        return NULL;
    }

    const char *thread_url = "https://api.openai.com/v1/threads";
    char *response = assistants_send_request(assistants->apiKey, thread_url, json_string);
    ESP_LOGI("HTTP", "Raw response: %s", response);
    cJSON_free(json_string);
    json_string = NULL;

    if (response == NULL)
    {
        ESP_LOGE("HTTP", "Request to create thread failed");
        free(response);
        return NULL;
    }

    // Parsear la respuesta para obtener el `id` del hilo
    cJSON *response_json = cJSON_Parse(response);
    free(response); // Ya no necesitamos la respuesta completa
    if (response_json == NULL)
    {
        ESP_LOGE("JSON", "Failed to parse response JSON");
        return NULL;
    }

    cJSON *id_item = cJSON_GetObjectItem(response_json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item))
    {
        ESP_LOGE("JSON", "Failed to get `id` from response");
        cJSON_Delete(response_json);
        return NULL;
    }

    char *thread_id = strdup(id_item->valuestring); // Copiar el `id` del hilo
    cJSON_Delete(response_json);

    ESP_LOGI("HTTP", "Thread created with ID: %s", thread_id);
    return thread_id; // Quien llame esta función debe liberar `thread_id` cuando ya no lo necesite
}

// Función para crear un mensaje
char *createMessage(Assistants *assistants, const char *threadId, const char *content)
{
    if (assistants == NULL || assistants->apiKey == NULL || threadId == NULL || content == NULL)
    {
        ESP_LOGE("HTTP", "Invalid parameters passed to createMessage()");
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.openai.com/v1/threads/%s/messages", threadId);

    // Crear estructura JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON structure");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(root, "role", "user");
    cJSON_AddStringToObject(root, "content", content);

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_string == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON string");
        return NULL;
    }

    // Imprimir el JSON string antes de enviar la solicitud
    ESP_LOGI("JSON", "Request JSON: %s", json_string);
    char *response = assistants_send_request(assistants->apiKey, url, json_string);
    cJSON_free(json_string);

    if (response == NULL)
    {
        ESP_LOGE("HTTP", "Request to create message failed");
        free(response);
        return NULL;
    }

    ESP_LOGI("HTTP", "Message created: %s", response);
    return response; // La memoria debe ser liberada por quien llame a esta función
}

// assistants.c

bool runAssistant(Assistants *assistants, const char *userThreadId, const char *assistantId)
{
    // Validación de parámetros
    if (assistants == NULL || assistants->apiKey == NULL || userThreadId == NULL || assistantId == NULL)
    {
        ESP_LOGE("HTTP", "Invalid parameters passed to runAssistant()");
        return false;
    }

    // Construir la URL para iniciar el run del assistant
    char url[256];
    snprintf(url, sizeof(url), "https://api.openai.com/v1/threads/%s/runs", userThreadId);

    // Crear el objeto JSON para la petición
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON object");
        return false;
    }
    cJSON_AddStringToObject(root, "assistant_id", assistantId);

    // Convertir el objeto JSON a una cadena sin formato
    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_string == NULL)
    {
        ESP_LOGE("JSON", "Failed to create JSON string");
        return false;
    }

    char *response = assistants_send_request(assistants->apiKey, url, json_string);
    cJSON_free(json_string);
    json_string = NULL;

    if (response == NULL)
    {
        ESP_LOGE("HTTP", "Request to run assistant failed");
        // No es necesario hacer free(response) aquí porque ya es NULL
        return false;
    }

    // Parsear la respuesta y obtener el run ID
    cJSON *response_json = cJSON_Parse(response);
    free(response); // Ya no necesitamos la respuesta completa
    if (response_json == NULL)
    {
        ESP_LOGE("JSON", "Failed to parse response JSON");
        return false;
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(response_json, "id");
    if (!cJSON_IsString(id_item) || id_item->valuestring == NULL)
    {
        ESP_LOGE("JSON", "Failed to get run ID");
        cJSON_Delete(response_json);
        return false;
    }

    const char *runId = id_item->valuestring;
    char status_url[256];
    snprintf(status_url, sizeof(status_url), "https://api.openai.com/v1/threads/%s/runs/%s", userThreadId, runId);
    cJSON_Delete(response_json); // Ya no necesitamos el JSON de la primera respuesta

    // Loop para consultar el estado del run hasta que se complete
    int is_completed = 0;
    int retry_count = 0;       // <<<--- AÑADIR CONTADOR DE REINTENTOS
    const int max_retries = 5; // <<<--- DEFINIR MÁXIMO DE REINTENTOS

    while (!is_completed && retry_count < max_retries)
    {
        response = assistants_send_request(assistants->apiKey, status_url, NULL); // Usar NULL para GET

        if (response != NULL)
        {
            cJSON *status_response_json = cJSON_Parse(response);
            free(response); // Liberar inmediatamente después del parseo
            if (status_response_json)
            {
                cJSON *status_item = cJSON_GetObjectItemCaseSensitive(status_response_json, "status");
                if (cJSON_IsString(status_item) && strcmp(status_item->valuestring, "completed") == 0)
                {
                    is_completed = 1;
                }
                cJSON_Delete(status_response_json);
            }
            retry_count = 0; // Reiniciar contador si la conexión fue exitosa
        }
        else
        {
            // Si la solicitud falla (response es NULL), incrementar el contador
            retry_count++;
            ESP_LOGW("HTTP", "Failed to get run status, retry %d/%d", retry_count, max_retries);
        }

        if (!is_completed)
        {
            vTaskDelay(pdMS_TO_TICKS(5000)); // Esperar 5 segundos antes de la siguiente verificación
        }
    }

    if (!is_completed)
    {
        ESP_LOGE("HTTP", "Assistant run failed to complete after %d retries.", max_retries);
        return false; // Indicar que el run falló
    }

    ESP_LOGI("HTTP", "Assistant run completed");
    return true; // Indicar que el run se completó exitosamente
}

// Función para listar los mensajes de un hilo
char *listMessages(Assistants *assistants, const char *threadId)
{
    if (assistants == NULL || assistants->apiKey == NULL || threadId == NULL)
    {
        ESP_LOGE("HTTP", "Invalid parameters passed to listMessages()");
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.openai.com/v1/threads/%s/messages", threadId);

    char *response = assistants_send_request(assistants->apiKey, url, NULL);
    if (response == NULL)
    {
        ESP_LOGE("HTTP", "Request to list messages failed");
        free(response);
        return NULL;
    }

    cJSON *response_json = cJSON_Parse(response);
    if (response_json == NULL)
    {
        ESP_LOGE("JSON", "Failed to parse response JSON");
        free(response);
        return NULL;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(response_json, "data");
    if (!cJSON_IsArray(data))
    {
        ESP_LOGE("JSON", "Invalid data format in response");
        goto cleanup;
    }

    if (cJSON_GetArraySize(data) == 0)
    {
        ESP_LOGE("JSON", "No messages found in response");
        goto cleanup;
    }

    cJSON *first_message = cJSON_GetArrayItem(data, 0);
    if (first_message == NULL)
    {
        ESP_LOGE("JSON", "Failed to get first message");
        goto cleanup;
    }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(first_message, "content");
    if (!cJSON_IsArray(content))
    {
        ESP_LOGE("JSON", "Invalid content format in first message");
        goto cleanup;
    }

    cJSON *first_content = cJSON_GetArrayItem(content, 0); // Asumimos que el primer item contiene el texto
    if (first_content == NULL)
    {
        ESP_LOGE("JSON", "Failed to get content from first message");
        goto cleanup;
    }

    cJSON *text = cJSON_GetObjectItemCaseSensitive(first_content, "text");
    if (!cJSON_IsObject(text))
    {
        ESP_LOGE("JSON", "Invalid text format in first message content");
        goto cleanup;
    }

    cJSON *value = cJSON_GetObjectItemCaseSensitive(text, "value");
    if (!cJSON_IsString(value))
    {
        ESP_LOGE("JSON", "Failed to get value from text");
        goto cleanup;
    }

    char *value_str = strdup(value->valuestring);
    if (value_str == NULL)
    {
        ESP_LOGE("Memory", "Failed to allocate memory for value_str");
        goto cleanup;
    }

    cJSON_Delete(response_json);
    free(response);
    return value_str;

cleanup:
    cJSON_Delete(response_json);
    free(response);
    return NULL;
}
