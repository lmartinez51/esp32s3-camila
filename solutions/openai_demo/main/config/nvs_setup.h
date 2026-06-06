// nvs_setup.h
#pragma once

/**
 * @file nvs_setup.h
 * @brief Configuración del almacenamiento NVS (Non-Volatile Storage) para el proyecto OpenAI Demo.
 *
 * Este archivo contiene las funciones necesarias para inicializar y borrar el almacenamiento NVS,
 * que se utiliza para guardar configuraciones persistentes como credenciales WiFi.
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "host/ble_hs.h"   // Para ble_addr_t
#include "host/ble_uuid.h" // Para ble_uuid128_t
#include "ble_device_control.h"

#define BLE_DEVICE_MAX_NAME_LEN 32  // Longitud máxima del nombre del dispositivo BLE
#define MAX_DEVICES_PER_LOCATION 10 // Máximo de dispositivos por ubicación (SSID)
#define DEVICE_KEY_MAX_LEN 16       // Longitud máxima de la clave única del dispositivo en NVS
#define MIN_HEAP_BYTES (30 * 1024)  // Mínimo de memoria libre para operaciones NVS

    typedef struct
    {
        ble_addr_t addr; // dirección BLE (NimBLE)
        char name[BLE_DEVICE_MAX_NAME_LEN];
        uint8_t device_type;        // << en NVS guardamos un byte, no un enum
        ble_uuid128_t service_uuid; // si prefieres bytes: uint8_t service_uuid[16]
        ble_uuid128_t char_uuid;    // idem
        uint8_t ssid_crc;           // CRC8 del SSID asociado
        bool requires_bonding;      // si el dispositivo requiere emparejamiento
    } device_profile_nvs_t;

    /**
     * @brief Guarda un perfil de dispositivo en NVS.
     *
     * Esta función guarda un perfil de dispositivo BLE en el almacenamiento NVS
     * utilizando una clave única basada en el SSID y la dirección MAC del dispositivo.
     *
     * @param ssid SSID de la red WiFi asociada al dispositivo.
     * @param profile Perfil del dispositivo a guardar.
     * @return esp_err_t ESP_OK si se guardó correctamente, otro código de error en caso contrario.
     */
    esp_err_t save_device_profile(const char *ssid, const device_profile_nvs_t *profile);

    /**
     * @brief Carga un perfil de dispositivo desde NVS.
     *
     * Esta función carga un perfil de dispositivo BLE desde el almacenamiento NVS
     * utilizando el SSID y la dirección MAC del dispositivo para buscarlo.
     *
     * @param ssid SSID de la red WiFi asociada al dispositivo.
     * @param mac Dirección MAC del dispositivo a cargar.
     * @param out Puntero donde se almacenará el perfil cargado.
     * @return esp_err_t ESP_OK si se cargó correctamente, otro código de error en caso contrario.
     */

    esp_err_t load_device_profile(const char *ssid, const uint8_t mac[6], device_profile_nvs_t *out);

    /**
     * @brief Carga los perfiles de dispositivos asociados a un SSID desde NVS.
     *
     * Esta función carga todos los perfiles de dispositivos guardados en NVS
     * que coinciden con el SSID proporcionado.
     *
     * @param ssid SSID de la red WiFi para filtrar los perfiles.
     * @param profiles Array donde se guardarán los perfiles cargados.
     * @param max_profiles Número máximo de perfiles a cargar.
     * @return int Número de perfiles cargados exitosamente.
     */
    int load_devices_for_ssid(const char *ssid, device_profile_nvs_t *profiles, int max_profiles);

    /* @brief Inicializa el almacenamiento NVS.
     *
     * Esta función debe ser llamada al inicio del programa para preparar el almacenamiento NVS.
     * Si la inicialización falla, se registrará un error en el log.
     *
     * @note Asegúrate de que el almacenamiento NVS esté configurado correctamente en tu proyecto.
     */
    void init_nvs(void);

    /**
     * @brief Borra el almacenamiento NVS y reinicia el dispositivo.
     *
     * Esta función elimina todos los datos almacenados en NVS, lo que es útil para
     * restablecer la configuración del dispositivo a su estado inicial.
     * Después de borrar NVS, el dispositivo se reinicia automáticamente.
     *
     * @note Esta operación es irreversible y eliminará todas las configuraciones guardadas.
     */
    void erase_nvs(void);

    /**
     * @brief Inicializa el mutex para acceso seguro a NVS.
     * Esta función crea un mutex si no existe ya, para asegurar que
     * el acceso a NVS sea seguro en entornos multitarea.
     */
    void nvs_setup_mutex_init(void);

    /**
     * @brief Elimina un perfil de dispositivo específico de NVS.
     *
     * Esta función elimina el perfil de un dispositivo BLE basado en su SSID y dirección MAC.
     *
     * @param ssid SSID de la red WiFi asociada al dispositivo.
     * @param mac Dirección MAC del dispositivo a eliminar.
     * @return esp_err_t ESP_OK si se eliminó correctamente, otro código de error en caso contrario.
     */
    esp_err_t delete_device_profile(const char *ssid, const uint8_t mac[6]);

    /**
     * @brief Lista todos los dispositivos Bluetooth guardados en NVS.
     */
    void list_all_ble_devices_from_nvs(void);

    /**
     * @brief Lista todos los dispositivos Bluetooth disponibles para un SSID específico.
     *
     * Esta función imprime en consola todos los dispositivos BLE guardados en NVS
     * asociados al SSID proporcionado. Diseñada para ser llamada desde app_main()
     * al inicio del programa.
     *
     * @param ssid SSID de la red WiFi actual para filtrar dispositivos por ubicación
     * @return int Número de dispositivos encontrados y listados
     */
    int list_available_ble_devices(const char *ssid);

    /**
     * @brief Lista todas las características BLE guardadas en NVS.
     * Esta función imprime en consola todas las características BLE
     * que han sido guardadas en NVS.
     */
    void list_all_characteristics_from_nvs(void);

    /**
     * @brief Obtiene el conteo de dispositivos BLE disponibles para un SSID específico.
     *
     * Esta función devuelve el número de dispositivos BLE registrados en NVS
     * asociados al SSID proporcionado.
     *
     * @param ssid SSID de la red WiFi actual para filtrar dispositivos por ubicación
     * @return int Número de dispositivos encontrados
     */
    int get_ble_device_count(const char *ssid);

    /**
     * @brief Lista los dispositivos BLE disponibles en formato JSON.
     *
     * Esta función genera una representación JSON de los dispositivos disponibles
     * que puede ser enviada al AI Chatbot para que conozca qué dispositivos puede controlar.
     *
     * @param ssid SSID de la red WiFi actual
     * @param json_buffer Buffer donde se escribirá el JSON
     * @param buffer_size Tamaño del buffer
     * @return int Número de dispositivos incluidos en el JSON
     */
    int list_devices_as_json(const char *ssid, char *json_buffer, size_t buffer_size);

    /**
     * @brief Guarda un dispositivo descubierto durante el escaneo en NVS.
     *
     * Esta función guarda un dispositivo BLE que ha sido descubierto durante
     * un escaneo y que cumple con los criterios para ser almacenado.
     *
     * @param device Puntero al dispositivo descubierto a guardar.
     * @return esp_err_t ESP_OK si se guardó correctamente, otro código de error en caso contrario.
     */
    esp_err_t save_discovered_device_to_nvs(const ble_device_info_t *device);

    /**
     * @brief Provisión de un dispositivo de prueba Philips Hue en NVS.
     *
     * Esta función crea y guarda un perfil de dispositivo simulado para pruebas,
     * representando una bombilla Philips Hue con valores predeterminados.
     *
     * @param ssid SSID de la red WiFi asociada al dispositivo de prueba.
     */
    void nvs_provision_hue_test_device(const char *ssid);

    /**
     * @brief Provisión de una base de datos inicial de perfiles conocidos en NVS.
     */
    void nvs_provision_known_profiles(void);

    void debug_nvs_contents(const char *ssid);

    void clean_invalid_ble_entries_from_nvs(void);

    void nvs_lock(void);
    void nvs_unlock(void);

    /**
     * @brief Valida el formato de una clave API de OpenAI.
     * Esta función verifica que la clave API proporcionada cumple con
     * los requisitos básicos de formato (longitud y prefijo).
     * @param api_key Cadena con la API Key a validar
     * @return esp_err_t ESP_OK si es válida, ESP_ERR_INVALID_ARG si no cumple los requisitos
     */
    esp_err_t validate_openai_api_key(const char *api_key);

    /**
     * @brief Guarda la clave API en NVS.
     * Esta función almacena de forma segura la clave API proporcionada
     * en el almacenamiento NVS para su uso posterior.
     * @param api_key Puntero a la cadena de la clave API a guardar.
     * @return esp_err_t ESP_OK si se guardó correctamente, otro código de error en caso contrario.
     */
    esp_err_t nvs_save_api_key(const char *api_key);

    /**
     * @brief Carga la clave API desde NVS.
     * Esta función recupera la clave API almacenada en NVS y la copia
     * en el buffer proporcionado.
     * @param out_buffer Buffer donde se almacenará la clave API cargada.
     * @param buffer_size Tamaño del buffer proporcionado.
     * @return esp_err_t ESP_OK si se cargó correctamente, otro código de error en caso contrario.
     */
    esp_err_t nvs_load_api_key(char *api_key_buffer, size_t buffer_size);

    /**
     * @brief Elimina la clave API almacenada en NVS.
     * Esta función borra la clave API guardada en NVS, si existe.
     * @return esp_err_t ESP_OK si se eliminó correctamente o no existía,
     * otro código de error en caso contrario.
     */
    esp_err_t nvs_delete_api_key(void);

    /**
     * @brief Lista la clave API almacenada en NVS (para depuración).
     * Esta función imprime en el log la clave API actualmente almacenada en NVS.
     * Útil para verificar que la clave se ha guardado correctamente.
     */
    void list_api_keys_from_nvs(void);

    /**
     * @brief Establece la bandera 'boot_to_provisioning' en NVS.
     * Esta función guarda una bandera en NVS que indica que el dispositivo
     * debe arrancar en modo de provisión en el próximo reinicio.
     */
    void nvs_set_boot_to_provisioning_flag(void);

    /**
     * @brief Lee y borra la bandera 'boot_to_provisioning' de NVS.
     * Esta función verifica si la bandera de provisión está establecida en NVS.
     * Si está presente, la borra para evitar arranques repetidos en modo provisión.
     * @return true si la bandera estaba establecida, false en caso contrario.
     */
    bool nvs_read_and_clear_boot_to_provisioning_flag(void);

#ifdef __cplusplus
}
#endif
