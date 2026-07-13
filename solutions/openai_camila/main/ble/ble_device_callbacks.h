/**
 * @file ble_device_callbacks.h
 * @brief BLE Device Control Callbacks for ESP32-S3-BOX3 AI Chatbot
 *
 * Este módulo contiene los callbacks específicos de la aplicación para
 * manejar eventos del sistema de control de dispositivos BLE.
 * Integra la funcionalidad BLE con el sistema AI Chatbot.
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_DEVICE_CALLBACKS_H
#define BLE_DEVICE_CALLBACKS_H

#include "ble_device_control.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Obtiene la estructura de callbacks configurada para la aplicación
     *
     * Esta función devuelve un puntero a la estructura de callbacks
     * preconfigurada para el AI Chatbot.
     *
     * @return Puntero a la estructura de callbacks
     */
    ble_device_callbacks_t *ble_device_get_app_callbacks(void);

    /**
     * @brief Callback llamado cuando se descubre un dispositivo BLE
     *
     * @param device Información del dispositivo descubierto
     */
    void on_device_discovered_callback(ble_device_info_t *device);

    /**
     * @brief Callback llamado cuando se conecta un dispositivo BLE
     *
     * @param device Información del dispositivo conectado
     */
    void on_device_connected_callback(ble_device_info_t *device);

    /**
     * @brief Callback llamado cuando se desconecta un dispositivo BLE
     *
     * @param device Información del dispositivo desconectado
     */
    void on_device_disconnected_callback(ble_device_info_t *device);

    /**
     * @brief Callback llamado con el resultado de un comando enviado
     *
     * @param device Dispositivo al que se envió el comando
     * @param success true si el comando fue exitoso, false si falló
     */
    void on_command_result_callback(ble_device_info_t *device, bool success);

    /**
     * @brief Callback llamado cuando el descubrimiento inteligente se detiene
     *
     * @param nuevos_dispositivos Número de nuevos dispositivos descubiertos en el ciclo
     */
    void on_discovery_stopped_callback(int nuevos_dispositivos);

#ifdef __cplusplus
}
#endif

#endif // BLE_DEVICE_CALLBACKS_H