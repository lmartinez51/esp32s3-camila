#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

// typedef struct
// {
//     char *type;    // e.g., \"approximate\"
//     char *country; // e.g., \"GB\"
//     char *city;    // e.g., \"London\"
//     char *region;  // e.g., \"London\"
// } user_location_t;
#include "webrtc.h" // Asegúrate de tener esta línea

// Esta función realiza una búsqueda web a través de la API de OpenAI.
// - request_input: La consulta del usuario (requerido).
// - user_location: Información de ubicación opcional (puede ser NULL).
// Retorna el contenido del mensaje (message.content[0].text) que se extrae del response.
// El caller es responsable de liberar la memoria del string retornado.
char *web_search(const char *request_input);

#endif // WEB_SEARCH_H
