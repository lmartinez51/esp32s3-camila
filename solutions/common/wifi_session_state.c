// wifi_session_state.c
#include "wifi_session_state.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SSID_LEN 32 // Longitud máxima según estándar 802.11

static SemaphoreHandle_t ssid_mutex = NULL;
static char connected_ssid[MAX_SSID_LEN + 1] = ""; // +1 para terminador nulo

/**
 * @brief Guarda el SSID de la conexión actual
 * @param ssid SSID de la red conectada (se hace copia interna)
 */
void wifi_session_set_connected_ssid(const char *ssid)
{
    if (!ssid || *ssid == '\0')
        return;

    // Crear mutex si es primera vez
    if (ssid_mutex == NULL)
    {
        ssid_mutex = xSemaphoreCreateMutex();
        if (!ssid_mutex)
            return;
    }

    if (xSemaphoreTake(ssid_mutex, portMAX_DELAY) == pdTRUE)
    {
        const size_t len = strnlen(ssid, MAX_SSID_LEN);
        memset(connected_ssid, 0, sizeof(connected_ssid));
        strncpy(connected_ssid, ssid, len);
        xSemaphoreGive(ssid_mutex);
    }
}

/**
 * @brief Obtiene el SSID de la conexión actual
 * @return SSID de la red conectada (NULL si no hay conexión)
 */
const char *wifi_session_get_connected_ssid()
{
    if (ssid_mutex == NULL)
        return "";

    const char *result = "";
    if (xSemaphoreTake(ssid_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        result = connected_ssid;
        xSemaphoreGive(ssid_mutex);
    }
    return result;
}

void wifi_session_clear_ssid()
{
    if (ssid_mutex && xSemaphoreTake(ssid_mutex, portMAX_DELAY) == pdTRUE)
    {
        memset(connected_ssid, 0, sizeof(connected_ssid));
        xSemaphoreGive(ssid_mutex);
    }
}