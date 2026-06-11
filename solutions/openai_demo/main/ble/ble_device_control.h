/**
 * @file ble_device_control.h
 * @brief BLE Device Control module for ESP32-S3-BOX3 AI Chatbot
 *
 * This module extends the existing BLE WiFi functionality to include
 * outbound BLE device control capabilities. It allows the AI Chatbot
 * to discover and control BLE-enabled IoT devices like lights, fans, etc.
 *
 * Key features:
 * - BLE device discovery and scanning
 * - Device pairing and connection management
 * - Command transmission to BLE devices
 * - Device database management
 * - Integration with existing NimBLE stack
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef BLE_DEVICE_CONTROL_H
#define BLE_DEVICE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "host/ble_gap.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Constants */
#define BLE_DEVICE_MAX_NAME_LEN 32
#define BLE_DEVICE_MAX_DEVICES 25
#define BLE_DEVICE_SCAN_TIMEOUT_MS 15000
#define BLE_DEVICE_CONNECT_TIMEOUT_MS 5000
#define BLE_DEVICE_CMD_TIMEOUT_MS 3000
#define BLE_IDENTITY_SERVICE_UUID_STR "0000AAAA-0000-1000-8000-00805F9B34FB"
#define BLE_IDENTITY_SERVICE_UUID_BYTES                         \
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,             \
    0x00, 0x10, 0x00, 0x00, 0xAA, 0xAA, 0x00, 0x00
#define BLE_IDENTITY_RSSI_ENTER_DBM (-65)
#define BLE_IDENTITY_RSSI_EXIT_DBM (-85)
// Describe el perfil completo de un tipo de dispositivo
#define MAX_CHARS_PER_PROFILE 5 // Permitir hasta 5 características por perfil

    /* Device Types */
    typedef enum
    {
        BLE_DEVICE_TYPE_UNKNOWN = 0,
        BLE_DEVICE_TYPE_LIGHT,
        BLE_DEVICE_TYPE_FAN,
        BLE_DEVICE_TYPE_VACUUM,
        BLE_DEVICE_TYPE_SPEAKER,
        BLE_DEVICE_TYPE_THERMOSTAT,
        BLE_DEVICE_TYPE_CUSTOM
    } ble_device_type_t;

    /* Device States */
    // Versión Simplificada del Enum de Estados
    typedef enum
    {
        BLE_DEVICE_STATE_DISCONNECTED,       // 0: Desconectado o recién descubierto
        BLE_DEVICE_STATE_CONNECTING,         // 1: Conectando...
        BLE_DEVICE_STATE_CONNECTED,          // 2: Conexión física establecida
        BLE_DEVICE_STATE_DISCOVERING_SVCS,   // 3: Descubriendo servicios...
        BLE_DEVICE_STATE_DISCOVERING_CHRS,   // 4: Descubriendo características...
        BLE_DEVICE_STATE_DISCOVERY_COMPLETE, // 5: Descubrimiento finalizado, perfil guardado
        BLE_DEVICE_STATE_ERROR               // 6: Ocurrió un error
    } ble_device_state_t;

    typedef enum
    {
        READ_SUCCESS = 0,
        READ_ERROR_SECURITY,
        READ_ERROR_INVALID_HANDLE,
        READ_ERROR_NOT_PERMITTED,
        READ_ERROR_TIMEOUT,
        READ_ERROR_CONNECTION_LOST,
        READ_ERROR_INSUFFICIENT_RESOURCES,
        READ_ERROR_UNKNOWN
    } read_error_type_t;

    /* Device Information Structure */
    typedef struct
    {
        uint8_t addr[6];
        uint8_t addr_type;
        char name[BLE_DEVICE_MAX_NAME_LEN];
        ble_device_type_t type;
        int8_t rssi;
        uint32_t last_seen;
        ble_device_state_t state;
        uint16_t conn_handle;

        // UUIDs y handle para control (pueden venir de NVS o ser descubiertos)
        ble_uuid128_t service_uuid_128;
        ble_uuid128_t char_uuid_128;
        uint16_t char_val_handle;

        // NUEVO CAMPO
        bool is_known;        // True si fue cargado desde NVS o ya fue aprendido
        bool char_discovered; // True si fue descubierto en el escaneo actual

        uint16_t pairing_service_start_handle;
        uint16_t pairing_service_end_handle;
        uint16_t pairing_char_handle;

        bool pairing_char_found; // true cuando encontramos la char de pairing
        bool control_char_found; // true cuando encontramos la char de control

        int matched_profile_index;

        bool processed_in_current_cycle; // true si ya fue procesado en el ciclo actual

    } ble_device_info_t;

    // Describe una característica clave con sus propiedades
    typedef struct
    {
        ble_uuid128_t uuid;
        uint8_t properties; // Aquí guardaremos las propiedades (READ, WRITE, etc.)
    } known_characteristic_profile_t;

    typedef struct
    {
        char profile_name[32];
        ble_uuid128_t service_uuid;
        uint8_t num_characteristics;
        known_characteristic_profile_t characteristics[MAX_CHARS_PER_PROFILE];
        ble_device_type_t device_type;
    } known_device_profile_t;

    /* Callback Types */
    typedef void (*ble_device_discovered_cb_t)(ble_device_info_t *device);
    typedef void (*ble_device_connected_cb_t)(ble_device_info_t *device);
    typedef void (*ble_device_disconnected_cb_t)(ble_device_info_t *device);
    typedef void (*ble_command_result_cb_t)(ble_device_info_t *device, bool success);
    typedef void (*ble_discovery_stopped_cb_t)(int nuevos_dispositivos);

    /* Callback Structure */
    typedef struct
    {
        ble_device_discovered_cb_t on_device_discovered;
        ble_device_connected_cb_t on_device_connected;
        ble_device_disconnected_cb_t on_device_disconnected;
        ble_command_result_cb_t on_command_result;
        ble_discovery_stopped_cb_t on_discovery_stopped;
    } ble_device_callbacks_t;

    /* Smart Discovery Metrics */
    typedef struct
    {
        uint32_t nuevos_dispositivos_en_ciclo; // siempre >= 0
        uint32_t ciclos_vacios_consecutivos;   // siempre >= 0
        TickType_t tiempo_ultimo_nuevo;        // en ticks de FreeRTOS
        uint32_t fallos_consecutivos;          // errores consecutivos
        bool primera_visita;                   // SSID nuevo vs conocido
    } ble_discovery_metrics_t;

    /* Public API Functions */

    /**
     * @brief Initialize BLE device control module
     *
     * @param callbacks Pointer to callback structure
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t ble_device_control_start(ble_device_callbacks_t *callbacks);

    /**
     * @brief Deinitialize BLE device control module
     */
    void ble_device_control_stop(void);

    /**
     * @brief Start scanning for BLE devices
     *
     * @param timeout_ms Scan timeout in milliseconds (0 for indefinite)
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t ble_device_start_scan(uint32_t timeout_ms);

    /**
     * @brief Stop scanning for BLE devices
     *
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t ble_device_stop_scan(void);

    /**
     * @brief Connect to a BLE device
     *
     * @param device_addr BLE device address
     * @param addr_type Address type
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t ble_device_connect(uint8_t device_addr[6], uint8_t addr_type);

    /**
     * @brief Disconnect from a BLE device
     *
     * @param device_addr BLE device address
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t ble_device_disconnect(uint8_t device_addr[6]);

    /**
     * @brief Get list of discovered devices
     *
     * @param devices Array to store device information
     * @param max_devices Maximum number of devices to return
     * @return int Number of devices returned
     */
    int ble_device_get_discovered_list(ble_device_info_t devices[], int max_devices);

    /**
     * @brief Find device by name
     *
     * @param name Device name to search for
     * @return ble_device_info_t* Pointer to device info or NULL if not found
     */
    ble_device_info_t *ble_device_find_by_name(const char *name);

    /**
     * @brief Find device by address
     *
     * @param addr Device address to search for
     * @return ble_device_info_t* Pointer to device info or NULL if not found
     */
    ble_device_info_t *ble_device_find_by_addr(uint8_t addr[6]);

    /**
     * @brief Get device type from name (heuristic)
     *
     * @param name Device name
     * @return ble_device_type_t Detected device type
     */
    ble_device_type_t ble_device_detect_type_from_name(const char *name);

    /**
     * @brief FASE 1 - Funciones de Auto-conexión Inteligente
     */

    /**
     * @brief Inicia descubrimiento inteligente con auto-conexión
     *
     * Esta función inicia un escaneo de 20 segundos y conecta automáticamente
     * a dispositivos de interés según los criterios configurados.
     *
     * @return ESP_OK si el escaneo se inició correctamente
     */
    esp_err_t ble_device_start_smart_discovery(uint32_t duration_ms);

    /**
     * @brief Habilita o deshabilita la auto-conexión globalmente
     *
     * @param enable true para habilitar, false para deshabilitar
     */
    void ble_device_set_auto_connection_enabled(bool enable);

    /**
     * @brief Configura los parámetros del descubrimiento inteligente
     *
     * Permite ajustar los intervalos de escaneo, número de dispositivos objetivo,
     * y si se envían comandos de prueba automáticamente.
     *
     * @param normal_interval_ms Intervalo en modo normal (ms)
     * @param maintenance_interval_ms Intervalo en modo mantenimiento (ms)
     * @param target_devices Número de dispositivos objetivo
     * @param enable_test_commands Habilitar comandos de prueba automáticos
     * @return ESP_OK si la configuración fue exitosa
     */
    esp_err_t ble_device_configure_smart_discovery(uint32_t normal_interval_ms,
                                                   uint32_t maintenance_interval_ms,
                                                   int target_devices,
                                                   bool enable_test_commands);
    /**
     * @brief FASE 2 - Funciones de Descubrimiento Inteligente Avanzado
     */

    /**
     * @brief Inicia la tarea de descubrimiento inteligente en segundo plano
     *
     * Crea una tarea que ejecuta ciclos de descubrimiento automático con
     * reintentos, manejo de errores y modos normal/mantenimiento.
     *
     * @return ESP_OK si la tarea se creó correctamente
     */
    esp_err_t ble_device_start_smart_task(void);

    /**
     * @brief Run a bounded passive BLE identity validation scan.
     *
     * Wakes the persistent smart BLE task for a strictly passive scan that only
     * accepts advertisements carrying BLE_IDENTITY_SERVICE_UUID_STR.
     *
     * @param timeout_ms Maximum validation window in milliseconds
     * @return ESP_OK if the validation task was scheduled
     */
    esp_err_t ble_device_start_identity_validation(uint32_t timeout_ms);

    esp_err_t ble_device_prepare_for_identity_scan(uint32_t timeout_ms);

    const char *ble_identity_get_last_validated_name(void);

    esp_err_t ble_device_full_release(uint32_t timeout_ms);

    /**
     * @brief Detiene la tarea de descubrimiento inteligente
     *
     * Detiene de forma segura la tarea en segundo plano y libera recursos.
     *
     * @return ESP_OK si se detuvo correctamente
     */
    esp_err_t ble_device_stop_smart_task(void);

    /**
     * @brief Inicializa el sistema de descubrimiento inteligente
     *
     * Configura todos los parámetros necesarios para el funcionamiento
     * del descubrimiento automático de dispositivos BLE.
     *
     * @return ESP_OK si se inicializó correctamente
     */
    esp_err_t ble_device_smart_system_init(void);

    /**
     * @brief Obtiene el número actual de dispositivos únicos descubiertos.
     * @return El número de dispositivos en la lista interna.
     */
    int ble_device_get_discovered_count(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_DEVICE_CONTROL_H
