#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Guarda las credenciales WiFi (SSID y contraseña) en la NVS.
     * Si la red ya existe (por SSID), solo se actualiza la contraseña.
     */
    esp_err_t network_save_wifi_credentials(const char *ssid, const char *password);

    /**
     * Obtiene las credenciales WiFi almacenadas en la NVS según el índice indicado.
     * @param index Índice de la red (0 a MAX_SAVED_NETWORKS-1).
     * @param ssid Buffer donde se almacenará el SSID (mínimo 32 bytes).
     * @param password Buffer donde se almacenará la contraseña (mínimo 64 bytes).
     * @return true si se pudieron leer las credenciales; false en caso contrario.
     */
    bool network_get_saved_credentials(int index, char *ssid, char *password);

    /**
     * Elimina las credenciales WiFi almacenadas para el SSID especificado.
     * @param ssid_to_delete SSID de la red cuyas credenciales se desean eliminar.
     * @return true si se eliminaron las credenciales; false si no se encontró el SSID.
     */
    bool network_delete_wifi_credential_by_ssid(const char *ssid_to_delete);

    /**
     * Obtiene todos los SSIDs almacenados en NVS y los concatena en un buffer.
     * Cada SSID está separado por un carácter de nueva línea ('\n').
     * @param buffer Buffer donde se almacenarán los SSIDs concatenados.
     * @param buffer_len Longitud del buffer proporcionado.
     * @return ESP_OK si se obtuvieron los SSIDs correctamente,
     *         ESP_ERR_NVS_NOT_FOUND si no hay SSIDs almacenados,
     *         otro código de error en caso de fallo.
     */
    esp_err_t network_get_all_ssids(char *buffer, size_t buffer_len);

    /**
     * Borra TODAS las credenciales WiFi almacenadas en NVS.
     * @return ESP_OK si el borrado fue exitoso, o un código de error en caso contrario.
     */
    esp_err_t network_delete_all_wifi_credentials(void);

#ifdef __cplusplus
}
#endif
