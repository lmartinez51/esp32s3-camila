#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C"
{
#endif

// --- Declaraciones Externas ---
// Se declara la variable como 'extern' para que otros archivos sepan que existe.
// La variable real será creada en main.c
extern EventGroupHandle_t app_startup_event_group;

// --- Definiciones de Bits ---
// Se definen los bits que usaremos como banderas en nuestro grupo de eventos.
#define WIFI_CONNECTED_BIT BIT0   // Bandera para cuando el WiFi tenga IP
#define WEBRTC_CONNECTED_BIT BIT1 // Bandera para cuando el canal de datos de WebRTC esté abierto
#define WEBRTC_API_ERROR_BIT BIT2 // Bandera para error de clave API en WebRTC
#define WIFI_DISCONNECTED_BIT BIT3 // Bandera para notificar perdida de WiFi
#define WEBRTC_DISCONNECTED_BIT BIT4 // Bandera para notificar caida de WebRTC
#define LUA_INIT_DONE_BIT BIT5 // Bandera para notificar que Lua completó su init en PSRAM


typedef enum
{
    ORCH_EVENT_WIFI_CONNECTED = 0,
    ORCH_EVENT_WIFI_DISCONNECTED,
    ORCH_EVENT_MOTION_DETECTED,
    ORCH_EVENT_BLE_READY,
    ORCH_EVENT_BLE_BUSY,
    ORCH_EVENT_IDENTITY_PRESENT,
    ORCH_EVENT_IDENTITY_REJECTED,
    ORCH_EVENT_BLE_RELEASE_COMPLETE,
    ORCH_EVENT_BLE_RELEASE_FAILED,
    ORCH_EVENT_WEBRTC_CONNECTED,
    ORCH_EVENT_WEBRTC_DISCONNECTED,
    ORCH_EVENT_WEBRTC_API_ERROR,
    ORCH_EVENT_WEBRTC_STOPPED,
    ORCH_EVENT_AUTO_SLEEP_TIMEOUT,
    ORCH_EVENT_ALERT_DISPATCH_COMPLETE,
    ORCH_EVENT_ALERT_DISPATCH_FAILED,
    ORCH_EVENT_VIGILANTE_ROOM_VACATED,
    ORCH_EVENT_VIGILANTE_TIMEOUT,
    ORCH_EVENT_MIC_MUTED,
    ORCH_EVENT_MIC_UNMUTED,
    ORCH_EVENT_IDLE_ALERT_START,
    ORCH_EVENT_IDLE_ALERT_END,
} orchestrator_event_t;

typedef struct
{
    orchestrator_event_t type;
    uint32_t timestamp_ms;
    float corr_drop;
} orchestrator_event_msg_t;

void orchestrator_post_event(orchestrator_event_t event);
void orchestrator_post_motion_detected(uint32_t timestamp_ms, float corr_drop);
void orchestrator_post_mute_state(bool is_muted);
bool orchestrator_get_mute_state(void);

#ifdef __cplusplus
}
#endif

#endif // APP_EVENTS_H
