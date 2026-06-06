/**
 * @file ble_wifi.h
 * @brief BLE WiFi Bridge - Configuración de WiFi mediante Bluetooth Low Energy
 *
 * Este módulo permite configurar credenciales WiFi a través de una conexión BLE.
 * El dispositivo actúa como un servidor BLE que expone un servicio personalizado
 * para recibir credenciales WiFi (SSID y contraseña) desde un cliente BLE.
 *
 * Características principales:
 * - Servicio BLE personalizado para configuración WiFi
 * - Coexistencia WiFi/BLE optimizada
 * - Validación de credenciales WiFi
 * - Manejo robusto de errores y timeouts
 * - Compatible con ESP-IDF 5.4
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 */

#ifndef BLE_WIFI_H
#define BLE_WIFI_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ========================================================================== */
/*                                CONSTANTES                                  */
/* ========================================================================== */

/**
 * @brief Nombre del dispositivo BLE que se mostrará en el advertising
 */
#define BLE_WIFI_DEVICE_NAME "DR_SIMI" //"AIM_CAMILA"

/**
 * @brief Longitud máxima permitida para el SSID WiFi
 */
#define BLE_WIFI_MAX_SSID_LEN 32

/**
 * @brief Longitud máxima permitida para la contraseña WiFi
 */
#define BLE_WIFI_MAX_PASSWORD_LEN 64

/**
 * @brief Longitud máxima de datos que se pueden recibir vía BLE
 */
#define BLE_MAX_RECEIVED_DATA_LEN 256

/**
 * @brief Timeout en milisegundos para la inicialización BLE
 */
#define BLE_WIFI_INIT_TIMEOUT_MS 15000

    /* ========================================================================== */
    /*                           FUNCIONES PÚBLICAS                              */
    /* ========================================================================== */

    /**
     * @brief Inicializa el módulo BLE WiFi Bridge
     *
     * Esta función realiza la inicialización completa del stack BLE, configura
     * los servicios GATT personalizados, establece la coexistencia WiFi/BLE
     * e inicia el advertising BLE.
     *
     * Secuencia de inicialización:
     * 1. Configuración de coexistencia WiFi/BLE
     * 2. Inicialización del puerto NimBLE
     * 3. Configuración de servicios GAP y GATT
     * 4. Registro de servicios GATT personalizados
     * 5. Configuración de callbacks
     * 6. Inicio del host task de NimBLE
     * 7. Espera de sincronización del stack
     * 8. Inicio del advertising
     *
     * @note Esta función es bloqueante y puede tardar hasta BLE_WIFI_INIT_TIMEOUT_MS
     *       en completarse. Se recomienda llamarla desde una tarea separada.
     *
     * @warning No llamar esta función múltiples veces sin antes llamar ble_wifi_deinit()
     *
     * @see ble_wifi_deinit()
     * @see ble_wifi_is_ready()
     */
    // void ble_wifi_init(void);

    /**
     * @brief Verifica si el módulo BLE WiFi está completamente inicializado y listo
     *
     * Esta función verifica que:
     * - El módulo BLE esté inicializado
     * - La sincronización del stack BLE esté completa
     * - El stack NimBLE esté sincronizado
     *
     * @return true si el módulo está completamente listo para recibir conexiones
     * @return false si el módulo no está listo o hay algún error
     *
     * @note Es recomendable verificar este estado antes de realizar operaciones
     *       que dependan del BLE, especialmente después de llamar ble_wifi_init()
     *
     * @see ble_wifi_init()
     */
    // bool ble_wifi_is_ready(void);

    /**
     * @brief Deinicializa el módulo BLE WiFi Bridge y libera recursos
     *
     * Esta función realiza una limpieza completa del módulo:
     * 1. Detiene el advertising BLE si está activo
     * 2. Cierra conexiones BLE existentes
     * 3. Detiene y deinicializa el puerto NimBLE
     * 4. Libera semáforos y recursos de memoria
     * 5. Resetea variables de estado
     *
     * @note Después de llamar esta función, es necesario llamar ble_wifi_init()
     *       nuevamente para volver a utilizar el módulo
     *
     * @warning Esta función debe ser llamada antes de reinicializar el módulo
     *          o antes del shutdown del sistema para evitar memory leaks
     *
     * @see ble_wifi_init()
     */
    // void ble_wifi_deinit(void);

    /**
     * @brief Inicia el advertising de BLE si el stack está listo.
     */
    void ble_wifi_start_advertising(void);

    /**
     * @brief Detiene el advertising de BLE.
     */
    // void ble_wifi_stop_advertising(void);
    void ble_wifi_provisioning_stop(void);

    /**
     * @brief Registra los servicios GATT personalizados para el módulo BLE WiFi
     *
     * Esta función configura el nombre del dispositivo y registra los servicios
     * GATT necesarios para la provisión de WiFi a través de BLE.
     *
     * @return ESP_OK si los servicios se registraron correctamente
     * @return ESP_FAIL si hubo un error al registrar los servicios
     */
    esp_err_t ble_wifi_register_services(void);

    /**
     * @brief Desactiva permanentemente el modo Provisioning BLE
     *
     * Esta función detiene el advertising BLE y marca el modo provisioning
     * como inactivo, evitando que se pueda reactivar.
     *
     * @note Esta función es útil para asegurar que el dispositivo no vuelva
     *       a entrar en modo provisioning después de haber sido configurado.
     */
    void ble_wifi_provisioning_deinit(void);

    /* ========================================================================== */
    /*                              INFORMACIÓN                                   */
    /* ========================================================================== */

    /**
     * @brief Formato de datos esperado por el servicio BLE
     *
     * Los datos deben enviarse al servicio BLE en el siguiente formato:
     * "SSID PASSWORD"
     *
     * Donde:
     * - SSID: Nombre de la red WiFi (máximo 32 caracteres)
     * - PASSWORD: Contraseña de la red WiFi (máximo 64 caracteres)
     * - Separador: Un espacio en blanco entre SSID y PASSWORD
     *
     * Ejemplo: "MiRedWiFi MiContraseña123"
     *
     * @note El SSID no puede estar vacío
     * @note La contraseña puede estar vacía para redes abiertas
     * @note Los datos totales no pueden exceder BLE_MAX_RECEIVED_DATA_LEN bytes
     */

    /**
     * @brief UUIDs del servicio BLE
     *
     * Servicio WiFi UUID: 123456789abcdeff123456789abcde00
     * Característica WiFi UUID: 123456789abcdeff123456789abcff01
     *
     * Estos UUIDs de 128 bits son personalizados para evitar colisiones
     * con otros servicios BLE estándar.
     */

    /**
     * @brief Estados de conexión BLE
     *
     * El módulo maneja automáticamente los siguientes estados:
     * - Advertising: El dispositivo está visible y acepta conexiones
     * - Conectado: Un cliente BLE está conectado
     * - Desconectado: Se reinicia automáticamente el advertising
     *
     * @note El dispositivo solo acepta una conexión BLE simultánea
     */

#ifdef __cplusplus
}
#endif

#endif /* BLE_WIFI_H */