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
#include "nvs_guard.h"     // nvs_lock/nvs_unlock/nvs_setup_mutex_init (common component)

#define BLE_DEVICE_MAX_NAME_LEN 32  // Longitud máxima del nombre del dispositivo BLE
#define MAX_DEVICES_PER_LOCATION 10 // Máximo de dispositivos por ubicación (SSID)
#define DEVICE_KEY_MAX_LEN 16       // Longitud máxima de la clave única del dispositivo en NVS
#define MIN_HEAP_BYTES (30 * 1024)  // Mínimo de memoria libre para operaciones NVS

    typedef struct
    {
        ble_addr_t addr; // dirección BLE (NimBLE)
        char name[BLE_DEVICE_MAX_NAME_LEN];
        char alias[BLE_DEVICE_MAX_NAME_LEN]; // Alias amigable asignado por usuario (ej: "Foco Sala")
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
     * @brief Elimina un perfil de dispositivo BLE de NVS buscando por MAC.
     *
     * Recorre todas las claves D_* del namespace `ble_devices` (de cualquier
     * SSID) y borra aquellas cuyo blob contenga la MAC indicada. No depende
     * del SSID activo en el momento del borrado.
     *
     * @param mac Dirección MAC del dispositivo a eliminar.
     * @return esp_err_t ESP_OK si se eliminó (o no existía), otro código de error en caso contrario.
     */
    esp_err_t delete_device_profile_by_mac(const uint8_t mac[6]);

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
     * @brief Encola un dispositivo para persistirlo en NVS de forma asíncrona.
     *
     * El guardado real se ejecuta en una tarea dedicada de baja prioridad con
     * pila interna. NUNCA ejecuta operaciones de flash en el contexto del
     * llamante (pc_task de WebRTC o callbacks de NimBLE), evitando el cuelgue
     * del pipeline de audio y la corrupción de pila.
     *
     * @param device Puntero al dispositivo a guardar (se copia en profundidad).
     * @return esp_err_t ESP_OK si el perfil fue encolado, otro código en caso de error.
     */
    esp_err_t nvs_save_discovered_device_async(const ble_device_info_t *device);

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

    void clean_invalid_ble_entries_from_nvs(void);

    /**
     * @brief Borra TODOS los perfiles de dispositivos BLE de NVS (claves D_*).
     * La lista RAM del modulo BLE no se toca; el borrado se aplica en el
     * proximo arranque.
     */
    void nvs_wipe_ble_devices(void);

    /**
     * @brief Borra la base de perfiles conocidos del namespace `ble_profiles`
     * (clave `profiles_db`). La lista RAM del modulo BLE no se toca; el
     * borrado se aplica en el proximo arranque.
     */
    void nvs_wipe_ble_profiles(void);

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

    /* ── Phase 1: Boot Operation Mode ──────────────────────────────────────
     * Persists whether the device should arm itself (CENTINELA) or run the
     * normal BLE-identity flow (DIRECTO) on the next power-on.
     *
     * NVS namespace : "system"
     * NVS key       : "op_mode"  (u8, 15-char limit observed)
     * Default       : BOOT_MODE_DIRECTO (key absent ≡ DIRECTO)
     * ─────────────────────────────────────────────────────────────────── */

    /**
     * @brief Persistent boot-routing flag stored in NVS.
     *
     * BOOT_MODE_DIRECTO   — Normal operation: skip radar, validate BLE on boot.
     * BOOT_MODE_CENTINELA — Armed mode: arm radar/CSI on boot; require motion
     *                       before BLE validation. Written when an intruder
     *                       session ends so the next power-on is pre-armed.
     */
    typedef enum {
        BOOT_MODE_DIRECTO   = 0, /**< Default / trusted-occupant boot. */
        BOOT_MODE_CENTINELA = 1, /**< Armed / post-intrusion boot.     */
    } boot_operation_mode_t;

    /**
     * @brief Persist the boot operation mode to NVS flash.
     *
     * Opens namespace "system", writes key "op_mode" as a u8, commits, and
     * closes. Safe to call from any FreeRTOS task context.
     *
     * @param mode  BOOT_MODE_DIRECTO or BOOT_MODE_CENTINELA.
     */
    void nvs_set_operation_mode(boot_operation_mode_t mode);

    /**
     * @brief Read the current boot operation mode from NVS.
     *
     * Returns BOOT_MODE_DIRECTO if the key is absent (fresh flash / erased NVS)
     * so the system always defaults to the safe, non-armed state.
     *
     * @return The stored boot_operation_mode_t value.
     */
    boot_operation_mode_t nvs_get_operation_mode(void);

#ifdef __cplusplus
}
#endif
