#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_webrtc.h" // Aquí se define esp_webrtc_handle_t
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

    // typedef struct
    // {
    //     const char *type;
    //     const char *country;
    //     const char *city;
    //     const char *region;
    // } user_location_t;

    // Estructura de contexto para la tarea de búsqueda web
    typedef struct
    {
        char *user;
        char *query;
        char *call_id;
        // user_location_t *location;
        esp_webrtc_handle_t webrtc;
    } web_search_task_ctx_t;

    // Definimos los tipos de acciones que nuestra nueva tarea puede hacer
    typedef enum
    {
        WEBRTC_ACTION_SEND_IDLE_PROMPT, // Enviar el prompt de inactividad
        WEBRTC_ACTION_PLAY_IDLE_ALERT,  // Reproducir alerta de inactividad
        WEBRTC_ACTION_NOTIFY_UNMUTE,    // Notificar unmute físico a OpenAI
        // ... (aquí podríamos añadir más acciones en el futuro)
    } webrtc_action_t;

    typedef enum {
        OUTFIT_CASUAL_BLACK,
        OUTFIT_ELEGANT_EVENING,
        OUTFIT_LEATHER_JACKET
    } camila_outfit_t;

    typedef enum {
        CAMILA_STATE_BOOT = 0,
        CAMILA_STATE_IDLE,
        CAMILA_STATE_LISTENING,
        CAMILA_STATE_THINKING,
        CAMILA_STATE_TALKING,
        CAMILA_STATE_HAPPY,
        CAMILA_STATE_MUTED,
        CAMILA_STATE_ALERT,
        CAMILA_STATE_SAD,
        CAMILA_STATE_SLEEP,
        CAMILA_STATE_MAX
    } camila_state_t;

    int sendEvent(const char *type, const char *text);
    char *get_web_info(const char *request);
    int send_function_output(const char *call_id, const char *output);

    // void free_user_location(user_location_t *location);
    // user_location_t *parse_user_location(const cJSON *user_location_item);


#define WEB_SEARCH_TASK_STACK_SIZE (8 * 1024)
#define WEB_SEARCH_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

    void start_web_search_task(const char *user, const char *query, const char *call_id, esp_webrtc_handle_t webrtc_handle);

    void webrtc_send_system_prompt(const char *text);
    int webrtc_inject_arrival_context(void);
    uint32_t webrtc_get_last_activity_ms(void);
    void webrtc_mark_activity(void);
    bool webrtc_realtime_is_busy(void);
    bool webrtc_is_server_generating(void);

    /**
     * @brief Publica una acción en la cola de acciones de WebRTC.
     */
    void webrtc_init_action_queue(void); // <-- Prototipo para iniciar la tarea/cola

    /**
     * @brief Publica una acción en la cola de acciones de WebRTC.
     *
     * @param action La acción a publicar.
     */
    void webrtc_post_action(webrtc_action_t action);

#ifdef __cplusplus
}
#endif
