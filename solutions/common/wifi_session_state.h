// wifi_session_state.h
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Guarda el SSID de la conexión actual
     * @param ssid SSID de la red conectada (se hace copia interna)
     */
    void wifi_session_set_connected_ssid(const char *ssid);

    /**
     * @brief Obtiene el SSID de la conexión actual
     * @return SSID de la red conectada (NULL si no hay conexión)
     */
    const char *wifi_session_get_connected_ssid();

    /**
     * @brief Limpia el SSID almacenado (usar al desconectar)
     */
    void wifi_session_clear_ssid();

#ifdef __cplusplus
}
#endif