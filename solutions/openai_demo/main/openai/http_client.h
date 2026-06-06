#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <esp_http_client.h>
#include "esp_err.h"

// Estructura de configuración de solicitud HTTP
typedef struct
{
    char *url;
    esp_http_client_method_t method; // Usamos el tipo proporcionado por ESP-IDF
    char *payload;                   // Para métodos como POST/PUT
    char *headers;                   // Encabezado adicional en formato "Clave: Valor"
    const char *api_key;             // Clave API para autenticación (si es necesario)
    int timeout_ms;                  // Tiempo de espera en milisegundos
} http_request_t;

// Estructura de respuesta HTTP
typedef struct
{
    char *response;
    int status_code;
} http_response_t;

// Función para realizar una solicitud HTTP
esp_err_t http_client_request(http_request_t *request, http_response_t *response);

#endif // HTTP_CLIENT_H
