/*
 * nvs_setup.c
 *
 * Configuración del almacenamiento NVS (Non-Volatile Storage) para el proyecto OpenAI Demo.
 * Este archivo contiene las funciones necesarias para inicializar y borrar el almacenamiento NVS,
 * que se utiliza para guardar configuraciones persistentes como credenciales WiFi.
 */

#include "nvs_setup.h"

// Sistema / FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_crc.h"

// Proyecto
#include "ble_device_control.h"
#include "wifi_session_state.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------
 * Constantes y configuraciones locales
 * ------------------------------------------------------------------------- */
static const char *TAG = "NVS";
static const char *NVS_NAMESPACE = "ble_devices"; // namespace donde guardamos perfiles de dispositivos
static SemaphoreHandle_t nvs_mutex = NULL;        // global mutex para acceso seguro a NVS

/* -------------------------------------------------------------------------
 * Prototipos/funciones estáticas (helpers internos)
 * ------------------------------------------------------------------------- */
static inline uint8_t ssid_crc8(const char *ssid);
static void build_device_key(const char *ssid, const uint8_t mac[6], char out_key[DEVICE_KEY_MAX_LEN]);
static const char *ble_device_type_to_string(ble_device_type_t type);

/* -------------------------------------------------------------------------
 * Implementación
 * ------------------------------------------------------------------------- */

/**
 * Inicializa NVS (debe llamarse al inicio del sistema)
 */
void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Error al inicializar la NVS. Borrando NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/**
 * Borra todo NVS y reinicia (útil para debugging o reseteo completo)
 */
void erase_nvs(void)
{
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "NVS borrado exitosamente. Reiniciando dispositivo...");
        esp_restart();
    }
    else
    {
        ESP_LOGE(TAG, "Fallo al borrar NVS: %s", esp_err_to_name(err));
    }
}

/**
 * Inicializa el mutex para acceso seguro a NVS
 */
void nvs_setup_mutex_init(void)
{
    if (nvs_mutex == NULL)
    {
        nvs_mutex = xSemaphoreCreateMutex();
        if (nvs_mutex == NULL)
        {
            ESP_LOGE(TAG, "❌ Error creando mutex para NVS");
        }
        else
        {
            ESP_LOGI(TAG, "✅ Mutex NVS inicializado correctamente");
        }
    }
}

/**
 * Funciones para lock/unlock de NVS
 */
void nvs_lock(void)
{
    if (nvs_mutex)
        xSemaphoreTake(nvs_mutex, portMAX_DELAY);
}

/**
 * Funciones para lock/unlock de NVS
 */
void nvs_unlock(void)
{
    if (nvs_mutex)
        xSemaphoreGive(nvs_mutex);
}

/* ----------------- Helpers internos ----------------- */

/**
 * Calcula un CRC8 del SSID para usar en validaciones.
 */
static inline uint8_t ssid_crc8(const char *ssid)
{
    if (!ssid || strlen(ssid) == 0)
    {
        ESP_LOGE(TAG, "SSID inválido para CRC");
        return 0;
    }
    if (strlen(ssid) >= NVS_NS_NAME_MAX_SIZE)
    {
        ESP_LOGE(TAG, "SSID demasiado largo para CRC");
        return 0;
    }
    return esp_crc8_le(0, (const uint8_t *)ssid, strlen(ssid));
}

/**
 * Construye la clave única para un dispositivo basado en SSID y MAC.
 * La clave tiene el formato: D_<3bytes_low_endian>_<crc>
 * Ejemplo: D_5CD15CF9_3A
 */
static void build_device_key(const char *ssid, const uint8_t mac[6], char out_key[DEVICE_KEY_MAX_LEN])
{
    if (!ssid || !mac || !out_key)
    {
        ESP_LOGE(TAG, "Argumentos inválidos para construir clave de dispositivo");
        return;
    }

    if (strlen(ssid) >= NVS_NS_NAME_MAX_SIZE)
    {
        ESP_LOGE(TAG, "SSID demasiado largo para NVS");
        return;
    }

    uint8_t crc = ssid_crc8(ssid);
    // Convención: D_<3bytes_low_endian>_<crc>
    snprintf(out_key, DEVICE_KEY_MAX_LEN, "D_%02X%02X%02X_%02X", mac[2], mac[1], mac[0], crc);
}

/**
 * Convierte el tipo de dispositivo BLE a una cadena legible.
 */
static const char *ble_device_type_to_string(ble_device_type_t type)
{
    switch (type)
    {
    case BLE_DEVICE_TYPE_UNKNOWN:
        return "Desconocido";
    case BLE_DEVICE_TYPE_LIGHT:
        return "\xF0\x9F\x92\xA1 Luz/LED"; // emoji + texto
    case BLE_DEVICE_TYPE_FAN:
        return "\xF0\x9F\x8C\x82 Ventilador";
    case BLE_DEVICE_TYPE_VACUUM:
        return "\xF0\x9F\xA7\xB9 Aspiradora";
    case BLE_DEVICE_TYPE_SPEAKER:
        return "\xF0\x9F\x94\x8A Altavoz";
    case BLE_DEVICE_TYPE_THERMOSTAT:
        return "\xE2\x9A\xA1\xEF\xB8\x8F Termostato";
    case BLE_DEVICE_TYPE_CUSTOM:
        return "⚙️ Personalizado";
    default:
        return "❓ No definido";
    }
}

/* ----------------- Persistencia de perfiles de dispositivo ----------------- */

/**
 * Guarda un perfil de dispositivo en NVS.
 */
esp_err_t save_device_profile(const char *ssid, const device_profile_nvs_t *profile_in)
{
    if (!ssid || !profile_in || strlen(ssid) == 0)
    {
        ESP_LOGE(TAG, "SSID o perfil inválido para guardar");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        nvs_unlock();
        return err;
    }

    device_profile_nvs_t profile = *profile_in;
    profile.ssid_crc = ssid_crc8(ssid);

    char key[DEVICE_KEY_MAX_LEN];
    build_device_key(ssid, profile.addr.val, key);

    err = nvs_set_blob(nvs_handle, key, &profile, sizeof(profile));
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Perfil guardado. key=%s ssid_crc=%02X", key, profile.ssid_crc);
    }
    else
    {
        ESP_LOGE(TAG, "Error guardando perfil (%s): %s", key, esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    nvs_unlock();
    return err;
}

/**
 * Guarda un dispositivo descubierto durante el escaneo en NVS.
 */
esp_err_t save_discovered_device_to_nvs(const ble_device_info_t *device)
{
    if (!device || !device->is_known || device->char_discovered == false)
    {
        ESP_LOGE(TAG, "Dispositivo no válido para guardar en NVS");
        return ESP_ERR_INVALID_ARG;
    }

    const char *current_ssid = wifi_session_get_connected_ssid();
    if (!current_ssid || strlen(current_ssid) == 0)
    {
        ESP_LOGE(TAG, "No hay SSID activo para guardar dispositivo");
        return ESP_ERR_INVALID_STATE;
    }

    device_profile_nvs_t profile = {0};
    strlcpy(profile.name, device->name, sizeof(profile.name));
    memcpy(profile.addr.val, device->addr, sizeof(profile.addr.val));
    profile.addr.type = device->addr_type;
    profile.device_type = device->type;
    profile.service_uuid = device->service_uuid_128;
    profile.char_uuid = device->char_uuid_128;
    profile.requires_bonding = true;

    return save_device_profile(current_ssid, &profile);
}

/**
 * Carga un perfil de dispositivo desde NVS. Directamente por SSID y MAC.
 */
esp_err_t load_device_profile(const char *ssid, const uint8_t addr[6], device_profile_nvs_t *out_profile)
{
    if (!ssid || !addr || !out_profile)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        nvs_unlock();
        return err;
    }

    char key[DEVICE_KEY_MAX_LEN];
    build_device_key(ssid, addr, key);

    size_t size = sizeof(*out_profile);
    err = nvs_get_blob(h, key, out_profile, &size);

    nvs_close(h);
    nvs_unlock();
    return err;
}

/**
 * Carga los perfiles de dispositivos asociados a un SSID desde NVS.
 */
/* Reemplazo robusto para load_devices_for_ssid() */
int load_devices_for_ssid(const char *ssid, device_profile_nvs_t *profiles, int max_profiles)
{
    if (!ssid || !profiles || max_profiles <= 0 || strlen(ssid) == 0)
    {
        ESP_LOGE(TAG, "load_devices_for_ssid: argumentos inválidos");
        return 0;
    }

    ESP_LOGI(TAG, "Cargando dispositivos para SSID: %s", ssid);

    // Verificar memoria antes de proceder
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < MIN_HEAP_BYTES * 2)
    {
        ESP_LOGW(TAG, "Memoria insuficiente para operación NVS: %zu bytes", free_heap);
        return 0;
    }

    nvs_setup_mutex_init();

    if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Timeout obteniendo mutex NVS");
        return 0;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open falló: %s", esp_err_to_name(err));
        xSemaphoreGive(nvs_mutex);
        return 0;
    }

    const uint8_t target_crc = ssid_crc8(ssid);
    int count = 0;
    int iter = 0;
    const int MAX_ITER = 100; // Límite conservador

    nvs_iterator_t it = NULL;
    esp_err_t find_err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);

    while (find_err == ESP_OK && count < max_profiles && iter < MAX_ITER)
    {
        iter++;

        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        device_profile_nvs_t temp_profile;
        size_t size = sizeof(temp_profile);
        esp_err_t err_blob = nvs_get_blob(h, info.key, &temp_profile, &size);

        if (err_blob == ESP_OK && size == sizeof(temp_profile))
        {
            if (temp_profile.ssid_crc == target_crc)
            {
                if (count < max_profiles)
                {
                    profiles[count++] = temp_profile;
                    ESP_LOGD(TAG, "Dispositivo encontrado: %s", temp_profile.name);
                }
            }
        }

        find_err = nvs_entry_next(&it);

        // Pequeña pausa para permitir que otras tareas se ejecuten
        if (iter % 10 == 0)
        {
            vTaskDelay(1);
        }
    }

    if (it)
    {
        nvs_release_iterator(it);
    }

    nvs_close(h);
    xSemaphoreGive(nvs_mutex);

    ESP_LOGI(TAG, "Carga completada. Encontrados %d dispositivos para SSID: %s", count, ssid);
    return count;
}

/**
 * Elimina un perfil de dispositivo específico de NVS.
 */
esp_err_t delete_device_profile(const char *ssid, const uint8_t mac[6])
{
    if (!ssid || strlen(ssid) == 0)
    {
        ESP_LOGE(TAG, "SSID inválido para eliminar perfil");
        return ESP_ERR_INVALID_ARG;
    }
    if (!mac || mac[0] == 0)
    {
        ESP_LOGE(TAG, "MAC inválida para eliminar perfil");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_lock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    char key[DEVICE_KEY_MAX_LEN];
    build_device_key(ssid, mac, key);

    err = nvs_erase_key(h, key);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
        ESP_LOGI(TAG, "Perfil eliminado: %s", key);
    }
    else
    {
        ESP_LOGW(TAG, "No existe clave para borrar: %s", key);
    }

    nvs_close(h);
    nvs_unlock();
    return err;
}

/* ----------------- Listados y salida JSON ----------------- */

/**
 * Lista todos los dispositivos Bluetooth guardados en NVS.
 */
void list_all_ble_devices_from_nvs(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "📋 No se pudo abrir el namespace '%s'", NVS_NAMESPACE);
        nvs_unlock();
        return;
    }

    ESP_LOGI(TAG, "📋 === Dispositivos BLE en NVS (todas las redes) ===");

    int total = 0;
    nvs_iterator_t it = NULL;
    esp_err_t find_err = nvs_entry_find("nvs", NVS_NAMESPACE, NVS_TYPE_BLOB, &it);

    while (find_err == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        device_profile_nvs_t profile;
        size_t size = sizeof(profile);
        if (nvs_get_blob(h, info.key, &profile, &size) == ESP_OK)
        {
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     profile.addr.val[5], profile.addr.val[4], profile.addr.val[3],
                     profile.addr.val[2], profile.addr.val[1], profile.addr.val[0]);

            ESP_LOGI(TAG, "   • Nombre: %s", profile.name);
            ESP_LOGI(TAG, "     MAC: %s", mac_str);
            ESP_LOGI(TAG, "     SSID CRC: 0x%02X", profile.ssid_crc);
            ESP_LOGI(TAG, "     Tipo: %s", ble_device_type_to_string(profile.device_type));
            total++;
        }

        find_err = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);
    nvs_close(h);
    nvs_unlock();

    ESP_LOGI(TAG, "📊 Total dispositivos guardados: %d", total);
    ESP_LOGI(TAG, "📋 === Fin listado de dispositivos BLE ===");
}

/**
 * Lista los dispositivos Bluetooth asociados al SSID actual.
 * Retorna el número de dispositivos listados.
 */
int list_available_ble_devices(const char *ssid)
{
    if (!ssid || strlen(ssid) == 0)
    {
        ESP_LOGE(TAG, "SSID inválido para listar dispositivos");
        return 0;
    }

    device_profile_nvs_t devices[MAX_DEVICES_PER_LOCATION];
    int device_count = load_devices_for_ssid(ssid, devices, MAX_DEVICES_PER_LOCATION);

    if (device_count == 0)
    {
        ESP_LOGI(TAG, "🔍 No hay dispositivos Bluetooth registrados para la ubicación: %s", ssid);
        return 0;
    }

    ESP_LOGI(TAG, "📱 ======================================");
    ESP_LOGI(TAG, "📱 DISPOSITIVOS BLUETOOTH DISPONIBLES");
    ESP_LOGI(TAG, "📱 Ubicación: %s", ssid);
    ESP_LOGI(TAG, "📱 Total encontrados: %d", device_count);
    ESP_LOGI(TAG, "📱 ======================================");

    for (int i = 0; i < device_count; i++)
    {
        const device_profile_nvs_t *device = &devices[i];
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 device->addr.val[5], device->addr.val[4], device->addr.val[3],
                 device->addr.val[2], device->addr.val[1], device->addr.val[0]);

        ESP_LOGI(TAG, "📱 [%d] Dispositivo: %s", i + 1, device->name[0] != '\0' ? (const char *)device->name : "Sin nombre");
        ESP_LOGI(TAG, "    📍 MAC: %s", mac_str);
        ESP_LOGI(TAG, "    🔋 Tipo: %s", ble_device_type_to_string((ble_device_type_t)device->device_type));
        ESP_LOGI(TAG, "    🔧 CRC SSID: %02X", device->ssid_crc);
        ESP_LOGI(TAG, "    ═══════════════════════════════════════");
    }

    ESP_LOGI(TAG, "📱 Listo para recibir comandos de control!");
    ESP_LOGI(TAG, "📱 ======================================");

    return device_count;
}

/**
 * Retorna el número de dispositivos Bluetooth asociados al SSID actual.
 */
int get_ble_device_count(const char *ssid)
{
    if (!ssid || strlen(ssid) == 0)
        return 0;
    device_profile_nvs_t temp_devices[MAX_DEVICES_PER_LOCATION];
    return load_devices_for_ssid(ssid, temp_devices, MAX_DEVICES_PER_LOCATION);
}

/**
 * Lista los dispositivos Bluetooth asociados al SSID actual en formato JSON.
 * Retorna el número de dispositivos listados.
 */
int list_devices_as_json(const char *ssid, char *json_buffer, size_t buffer_size)
{
    if (!ssid || !json_buffer || buffer_size < 100)
        return 0;

    device_profile_nvs_t devices[MAX_DEVICES_PER_LOCATION];
    int device_count = load_devices_for_ssid(ssid, devices, MAX_DEVICES_PER_LOCATION);

    if (device_count == 0)
    {
        snprintf(json_buffer, buffer_size, "{\"location\":\"%s\",\"devices\":[]}", ssid);
        return 0;
    }

    int offset = snprintf(json_buffer, buffer_size, "{\"location\":\"%s\",\"device_count\":%d,\"devices\":[",
                          ssid, device_count);

    for (int i = 0; i < device_count && offset < (int)buffer_size - 50; i++)
    {
        const device_profile_nvs_t *device = &devices[i];
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 device->addr.val[5], device->addr.val[4], device->addr.val[3],
                 device->addr.val[2], device->addr.val[1], device->addr.val[0]);

        offset += snprintf(json_buffer + offset, buffer_size - offset,
                           "%s{\"name\":\"%s\",\"mac\":\"%s\",\"type\": \"%s\",\"ssid_crc\":\"%02X\",\"id\":%d}",
                           i > 0 ? "," : "",
                           device->name[0] != '\0' ? (const char *)device->name : "Unknown",
                           mac_str,
                           ble_device_type_to_string((ble_device_type_t)device->device_type),
                           device->ssid_crc,
                           i + 1);
    }

    snprintf(json_buffer + offset, buffer_size - offset, "]}");
    return device_count;
}

/**
 * Lista todas las características BLE guardadas en NVS.
 */
void list_all_characteristics_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ble_profiles", NVS_READONLY, &h); // Namespace correcto
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "📋 No se pudo abrir NVS para características: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "📋 === Características BLE en NVS ===");

    // Cargar la base de datos de perfiles
    known_device_profile_t profiles_db[10]; // Ajusta el tamaño según necesites
    size_t required_size = sizeof(profiles_db);

    err = nvs_get_blob(h, "profiles_db", profiles_db, &required_size);
    if (err == ESP_OK)
    {
        int profile_count = required_size / sizeof(known_device_profile_t);
        ESP_LOGI(TAG, "📋 Encontrados %d perfiles de dispositivos:", profile_count);

        for (int i = 0; i < profile_count; i++)
        {
            const known_device_profile_t *profile = &profiles_db[i];
            ESP_LOGI(TAG, "📋 Perfil [%d]: %s", i + 1, profile->profile_name);
            ESP_LOGI(TAG, "    🔧 Tipo: %s", ble_device_type_to_string(profile->device_type));

            // Mostrar UUID del servicio
            ESP_LOGI(TAG, "    📡 Servicio UUID:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, profile->service_uuid.value, 16, ESP_LOG_INFO);

            // Listar características
            ESP_LOGI(TAG, "    📋 Características (%d):", profile->num_characteristics);
            for (int j = 0; j < profile->num_characteristics; j++)
            {
                const known_characteristic_profile_t *chr = &profile->characteristics[j];
                ESP_LOGI(TAG, "      [%d] UUID:", j + 1);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, chr->uuid.value, 16, ESP_LOG_INFO);
                ESP_LOGI(TAG, "          Propiedades: 0x%02X", chr->properties);

                // Decodificar propiedades
                char prop_str[128] = {0};
                if (chr->properties & BLE_GATT_CHR_PROP_READ)
                    strcat(prop_str, "READ ");
                if (chr->properties & BLE_GATT_CHR_PROP_WRITE)
                    strcat(prop_str, "WRITE ");
                if (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)
                    strcat(prop_str, "NOTIFY ");
                if (chr->properties & BLE_GATT_CHR_PROP_INDICATE)
                    strcat(prop_str, "INDICATE ");
                ESP_LOGI(TAG, "          (%s)", prop_str[0] ? prop_str : "Sin propiedades");
            }
            ESP_LOGI(TAG, "    ╚══════════════════════════════════════");
        }
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "📋 No se encontró la base de datos de perfiles en NVS");
    }
    else
    {
        ESP_LOGE(TAG, "📋 Error cargando perfiles: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "📋 === Fin listado de características ===");
    nvs_close(h);
}

/* ----------------- Provisioning (base de datos conocida) ----------------- */

/**
 * Guarda un perfil de prueba para un dispositivo Hue en NVS.
 */
void nvs_provision_hue_test_device(const char *ssid)
{
    ESP_LOGW("NVS_SETUP", "--- Ejecutando Provisioning de Prueba para Foco Hue ---");

    const char *device_name = "Philips Hue White Lamp";
    const uint8_t device_addr_val[6] = {0xF9, 0x59, 0xA9, 0xD1, 0x5C, 0x2E};

    device_profile_nvs_t profile_to_save = {0};
    strlcpy(profile_to_save.name, device_name, sizeof(profile_to_save.name));
    memcpy(profile_to_save.addr.val, device_addr_val, sizeof(profile_to_save.addr.val));
    profile_to_save.addr.type = BLE_ADDR_RANDOM;
    profile_to_save.device_type = BLE_DEVICE_TYPE_LIGHT;
    profile_to_save.requires_bonding = true;

    const uint8_t service_uuid_bytes[16] = {0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83, 0xa2, 0x47, 0x00, 0x00, 0xbd, 0x32, 0x2c, 0x93};
    const uint8_t char_uuid_bytes[16] = {0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83, 0xa2, 0x47, 0x02, 0x00, 0xbd, 0x32, 0x2c, 0x93};

    ble_uuid_init_from_buf((ble_uuid_any_t *)&profile_to_save.service_uuid, service_uuid_bytes, 16);
    ble_uuid_init_from_buf((ble_uuid_any_t *)&profile_to_save.char_uuid, char_uuid_bytes, 16);

    esp_err_t ret = save_device_profile(ssid, &profile_to_save);
    if (ret == ESP_OK)
    {
        ESP_LOGW("NVS_SETUP", "✅ Perfil de prueba para '%s' guardado en NVS para el SSID '%s'", device_name, ssid);
    }
    else
    {
        ESP_LOGE("NVS_SETUP", "❌ Error al guardar el perfil de prueba para '%s'", device_name);
    }
}

/**
 * Guarda una base de datos de perfiles conocidos en NVS.
 * Actualmente solo incluye un perfil para Philips Hue White.
 */
void nvs_provision_known_profiles(void)
{
    ESP_LOGW("NVS_SETUP", "--- Ejecutando Provisioning de la Base de Datos de Perfiles ---");

    known_device_profile_t profiles_db[1];
    int profile_count = 0;

    known_device_profile_t *hue_profile = &profiles_db[profile_count++];
    strlcpy(hue_profile->profile_name, "Philips Hue White Lamp", sizeof(hue_profile->profile_name));
    hue_profile->device_type = BLE_DEVICE_TYPE_LIGHT;

    const uint8_t service_uuid_bytes[16] = {0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83, 0xa2, 0x47, 0x00, 0x00, 0xbd, 0x32, 0x2c, 0x93};
    ble_uuid_init_from_buf((ble_uuid_any_t *)&hue_profile->service_uuid, service_uuid_bytes, 16);

    hue_profile->num_characteristics = 0;

    known_characteristic_profile_t *on_off_char = &hue_profile->characteristics[hue_profile->num_characteristics++];
    const uint8_t on_off_uuid_bytes[16] = {0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83, 0xa2, 0x47, 0x02, 0x00, 0xbd, 0x32, 0x2c, 0x93};
    ble_uuid_init_from_buf((ble_uuid_any_t *)&on_off_char->uuid, on_off_uuid_bytes, 16);
    on_off_char->properties = BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_READ;

    known_characteristic_profile_t *brightness_char = &hue_profile->characteristics[hue_profile->num_characteristics++];
    const uint8_t brightness_uuid_bytes[16] = {0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83, 0xa2, 0x47, 0x03, 0x00, 0xbd, 0x32, 0x2c, 0x93};
    ble_uuid_init_from_buf((ble_uuid_any_t *)&brightness_char->uuid, brightness_uuid_bytes, 16);
    brightness_char->properties = BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_READ;

    nvs_handle_t nvs_handle;
    if (nvs_open("ble_profiles", NVS_READWRITE, &nvs_handle) != ESP_OK)
    {
        ESP_LOGE("NVS_SETUP", "Error abriendo NVS para perfiles");
        return;
    }

    esp_err_t ret = nvs_set_blob(nvs_handle, "profiles_db", profiles_db, sizeof(known_device_profile_t) * profile_count);
    if (ret == ESP_OK)
    {
        nvs_commit(nvs_handle);
        ESP_LOGW("NVS_SETUP", "✅ Base de datos con %d perfiles guardada en NVS.", profile_count);
    }
    else
    {
        ESP_LOGE("NVS_SETUP", "❌ Error guardando la base de datos de perfiles en NVS: %s", esp_err_to_name(ret));
    }
    nvs_close(nvs_handle);
}

/* ----------------- Debug / limpieza ----------------- */

/**
 * Función de debug para listar todo el contenido de NVS y verificar CRCs.
 */
void debug_nvs_contents(const char *ssid)
{
    ESP_LOGW("DEBUG", "=== DIAGNÓSTICO NVS ===");
    ESP_LOGW("DEBUG", "SSID buscado: '%s'", ssid);

    uint8_t target_crc = ssid_crc8(ssid);
    ESP_LOGW("DEBUG", "CRC calculado para '%s': 0x%02X", ssid, target_crc);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE("DEBUG", "Error abriendo namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    nvs_iterator_t it = NULL;
    err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW("DEBUG", "No se encontraron entradas.");
        nvs_close(h);
        return;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE("DEBUG", "Error en nvs_entry_find: %s", esp_err_to_name(err));
        nvs_close(h);
        return;
    }

    int total_entries = 0;
    while (it)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        total_entries++;

        ESP_LOGW("DEBUG", "Entrada encontrada: clave='%s'", info.key);

        device_profile_nvs_t tmp;
        size_t sz = sizeof(tmp);
        esp_err_t err_blob = nvs_get_blob(h, info.key, &tmp, &sz);
        if (err_blob == ESP_OK && sz == sizeof(tmp))
        {
            ESP_LOGW("DEBUG", "  └─ Dispositivo: %s", tmp.name);
            ESP_LOGW("DEBUG", "  └─ Tipo: %s", ble_device_type_to_string(tmp.device_type));
            ESP_LOGW("DEBUG", "  └─ CRC guardado: 0x%02X", tmp.ssid_crc);
            ESP_LOGW("DEBUG", "  └─ ¿Coincide CRC? %s", (tmp.ssid_crc == target_crc) ? "SÍ" : "NO");

            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     tmp.addr.val[5], tmp.addr.val[4], tmp.addr.val[3],
                     tmp.addr.val[2], tmp.addr.val[1], tmp.addr.val[0]);
            ESP_LOGW("DEBUG", "  └─ MAC: %s", mac_str);
        }
        else
        {
            ESP_LOGE("DEBUG", "  └─ Error leyendo blob: %s (tamaño=%zu)", esp_err_to_name(err_blob), sz);
        }

        esp_err_t err_next = nvs_entry_next(&it);
        if (err_next != ESP_OK && err_next != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE("DEBUG", "Error avanzando iterador: %s", esp_err_to_name(err_next));
            break;
        }
    }

    nvs_release_iterator(it);
    nvs_close(h);
    ESP_LOGW("DEBUG", "Total entradas encontradas: %d", total_entries);
    ESP_LOGW("DEBUG", "=== FIN DIAGNÓSTICO ===");
}

/**
 * Limpia entradas inválidas en NVS (tamaños incorrectos o blobs corruptos).
 */
void clean_invalid_ble_entries_from_nvs(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        nvs_unlock();
        return;
    }

    nvs_iterator_t it = NULL;
    esp_err_t find_err = nvs_entry_find("nvs", NVS_NAMESPACE, NVS_TYPE_BLOB, &it);

    while (find_err == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        device_profile_nvs_t profile;
        size_t size = sizeof(profile);
        err = nvs_get_blob(h, info.key, &profile, &size);

        if (err != ESP_OK || size != sizeof(profile))
        {
            ESP_LOGW(TAG, "⚠️ Eliminando entrada inválida: %s", info.key);
            nvs_erase_key(h, info.key);
        }

        find_err = nvs_entry_next(&it);
    }

    nvs_commit(h);
    nvs_release_iterator(it);
    nvs_close(h);
    nvs_unlock();

    ESP_LOGI(TAG, "✅ Limpieza de entradas inválidas completada.");
}

/**
 * @brief Valida el formato y longitud de una API Key de OpenAI.
 *
 * Las API Keys de OpenAI tienen formatos específicos según su tipo:
 * - API Keys de proyecto: "sk-proj-XXXXXXXX..." (164 caracteres total aprox.)
 * - API Keys legacy: "sk-XXXXXXXX..." (51 caracteres total aprox.)
 *
 * @param api_key Cadena con la API Key a validar
 * @return ESP_OK si es válida, ESP_ERR_INVALID_ARG si no cumple los requisitos
 */
esp_err_t validate_openai_api_key(const char *api_key)
{
    if (!api_key)
    {
        ESP_LOGE(TAG, "❌ API Key es NULL");
        return ESP_ERR_INVALID_ARG;
    }

    size_t key_len = strlen(api_key);

    // Validar longitud mínima. Una key más corta normalmente indica truncamiento
    // BLE, especialmente con write-without-response y MTU por defecto.
    if (key_len < 48)
    {
        ESP_LOGE(TAG, "❌ API Key demasiado corta (%zu caracteres). Mínimo: 48", key_len);
        return ESP_ERR_INVALID_ARG;
    }

    // Validar longitud máxima (con margen de seguridad)
    if (key_len > 200)
    {
        ESP_LOGE(TAG, "❌ API Key demasiado larga (%zu caracteres). Máximo: 200", key_len);
        return ESP_ERR_INVALID_ARG;
    }

    // Validar que comience con "sk-" (todas las API Keys de OpenAI empiezan así)
    if (strncmp(api_key, "sk-", 3) != 0)
    {
        ESP_LOGE(TAG, "❌ API Key no comienza con 'sk-'. Formato inválido.");
        return ESP_ERR_INVALID_ARG;
    }

    // Validar formato específico de API Keys de proyecto
    if (strncmp(api_key, "sk-proj-", 8) == 0)
    {
        // API Keys de proyecto tienen aproximadamente 164 caracteres
        if (key_len < 160)
        {
            ESP_LOGE(TAG, "❌ API Key de proyecto incompleta (%zu caracteres). Se esperan ~164.", key_len);
            ESP_LOGE(TAG, "   Probable truncamiento BLE/MTU. No se guardará en NVS.");
            return ESP_ERR_INVALID_ARG;
        }

        if (key_len > 170)
        {
            ESP_LOGW(TAG, "⚠️ API Key de proyecto con longitud inusual: %zu caracteres", key_len);
            ESP_LOGW(TAG, "   Se esperan ~164 caracteres. Verifica que esté completa.");
            // Solo advertencia por compatibilidad si OpenAI amplía el formato.
        }
        else
        {
            ESP_LOGI(TAG, "✅ API Key de proyecto con formato válido (%zu caracteres)", key_len);
        }
    }
    // Validar formato de API Keys legacy
    else if (key_len >= 48 && key_len <= 55)
    {
        ESP_LOGI(TAG, "✅ API Key legacy con formato válido (%zu caracteres)", key_len);
    }
    else
    {
        ESP_LOGW(TAG, "⚠️ API Key con formato desconocido pero válido prefijo 'sk-' (%zu caracteres)", key_len);
    }

    // Validar que solo contenga caracteres permitidos (base64-like + guiones)
    // OpenAI usa: letras, números, guiones y guiones bajos
    for (size_t i = 0; i < key_len; i++)
    {
        char c = api_key[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
        {
            ESP_LOGE(TAG, "❌ API Key contiene carácter inválido en posición %zu: '%c' (0x%02X)",
                     i, c, (unsigned char)c);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Verificar que no haya espacios en blanco (error común de copy-paste)
    if (strchr(api_key, ' ') != NULL || strchr(api_key, '\t') != NULL ||
        strchr(api_key, '\r') != NULL || strchr(api_key, '\n') != NULL)
    {
        ESP_LOGE(TAG, "❌ API Key contiene espacios en blanco o caracteres de control");
        return ESP_ERR_INVALID_ARG;
    }

    // ESP_LOGI(TAG, "✅ API Key validada exitosamente");
    return ESP_OK;
}

/**
 * @brief Guarda una API Key de OpenAI en la NVS después de validarla.
 *
 * Esta función realiza validaciones exhaustivas antes de guardar:
 * - Verifica que no sea NULL
 * - Valida el formato de OpenAI (debe comenzar con "sk-")
 * - Verifica la longitud según el tipo de API Key
 * - Comprueba que no contenga caracteres inválidos
 *
 * @param api_key Cadena con la API Key a guardar (debe ser válida)
 * @return ESP_OK si se guardó correctamente, código de error en caso contrario
 */
esp_err_t nvs_save_api_key(const char *api_key)
{
    if (!api_key)
    {
        ESP_LOGE(TAG, "❌ API Key inválida para guardar (NULL)");
        return ESP_ERR_INVALID_ARG;
    }

    // VALIDACIÓN PREVIA: Verificar formato antes de intentar guardar
    esp_err_t validation_result = validate_openai_api_key(api_key);
    if (validation_result != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ API Key no pasó la validación. No se guardará en NVS.");
        return validation_result;
    }

    // Inicializar y bloquear mutex
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error abriendo NVS namespace 'settings': %s", esp_err_to_name(err));
        nvs_unlock();
        return err;
    }

    // Log de información (sin mostrar la key completa por seguridad)
    size_t key_len = strlen(api_key);
    ESP_LOGI(TAG, "💾 Guardando API Key en NVS...");
    ESP_LOGI(TAG, "   Longitud: %zu caracteres", key_len);

    // Guardar la API Key como string
    err = nvs_set_str(nvs_handle, "openai_key", api_key);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error guardando API Key en NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        nvs_unlock();
        return err;
    }

    // Confirmar (commit) los cambios
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error haciendo commit de la API Key en NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        nvs_unlock();
        return err;
    }

    ESP_LOGI(TAG, "✅ API Key guardada exitosamente en NVS.");

    // Verificar que se guardó correctamente (lectura de verificación)
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "openai_key", NULL, &required_size);
    if (err == ESP_OK && required_size == (key_len + 1)) // +1 por null terminator
    {
        ESP_LOGI(TAG, "✅ Verificación: API Key almacenada correctamente (%zu bytes)", required_size);
    }
    else
    {
        ESP_LOGW(TAG, "⚠️ Advertencia: No se pudo verificar el almacenamiento completo");
    }

    // Cerrar handle y liberar mutex
    nvs_close(nvs_handle);
    nvs_unlock();

    return ESP_OK;
}

/**
 * @brief Lee la API Key de OpenAI almacenada en NVS.
 *  * Realiza validaciones exhaustivas después de leer:
 * - Verifica que el buffer de salida sea válido
 * - Comprueba que la key no esté vacía
 * - Valida el formato de OpenAI (debe comenzar con "sk-")
 * - Verifica la longitud según el tipo de API Key
 * - Comprueba que no contenga caracteres inválidos
 * Si la key es inválida, la elimina de NVS para evitar reintentos con datos corruptos.
 * @param out_buffer Buffer donde se almacenará la API Key leída
 * @param buffer_size Tamaño del buffer de salida
 * @return ESP_OK si se leyó y validó correctamente, código de error en caso contrario
 */
esp_err_t nvs_load_api_key(char *out_buffer, size_t buffer_size)
{
    if (!out_buffer || buffer_size == 0)
    {
        ESP_LOGE(TAG, "❌ Buffer inválido para leer API Key");
        return ESP_ERR_INVALID_ARG;
    }

    // Inicializar buffer por seguridad
    memset(out_buffer, 0, buffer_size);

    ESP_LOGI(TAG, "📖 Intentando leer API Key desde NVS...");

    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "⚠️ No se pudo abrir namespace 'settings': %s", esp_err_to_name(err));
        nvs_unlock();
        return err;
    }

    // Primero, obtener el tamaño requerido
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "openai_key", NULL, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "⚠️ No se encontró ninguna API Key en NVS.");
        nvs_close(nvs_handle);
        nvs_unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error al verificar tamaño de API Key: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        nvs_unlock();
        return err;
    }

    // ESP_LOGI(TAG, "   API Key encontrada: %zu bytes (incluye null terminator)", required_size);

    // Verificar que quepa en el buffer
    if (required_size > buffer_size)
    {
        ESP_LOGE(TAG, "❌ Buffer muy pequeño: necesita %zu bytes, disponible %zu",
                 required_size, buffer_size);
        nvs_close(nvs_handle);
        nvs_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    // Leer la API Key
    size_t out_size = buffer_size;
    err = nvs_get_str(nvs_handle, "openai_key", out_buffer, &out_size);

    // CERRAR y LIBERAR MUTEX ANTES de cualquier operación adicional
    nvs_close(nvs_handle);
    nvs_unlock();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error al leer API Key de NVS: %s", esp_err_to_name(err));
        memset(out_buffer, 0, buffer_size);
        return err;
    }

    // Validar que se leyó algo (no string vacío)
    if (out_buffer[0] == '\0')
    {
        ESP_LOGE(TAG, "❌ API Key leída está vacía");
        memset(out_buffer, 0, buffer_size);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t key_length = strlen(out_buffer);
    // ESP_LOGI(TAG, "   API Key leída: %zu caracteres", key_length);

    // Validar el formato - IMPORTANTE: Ya sin el mutex tomado
    err = validate_openai_api_key(out_buffer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ API Key en NVS tiene formato INVÁLIDO (%zu caracteres)", key_length);

        // ⚠️ AHORA SÍ podemos llamar a delete de forma segura (sin mutex tomado)
        ESP_LOGW(TAG, "🗑️ Borrando API Key corrupta de NVS...");
        esp_err_t delete_err = nvs_delete_api_key();
        if (delete_err != ESP_OK && delete_err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG, "❌ Error al borrar API Key corrupta: %s", esp_err_to_name(delete_err));
        }

        // Limpiar el buffer por seguridad
        memset(out_buffer, 0, buffer_size);

        return ESP_ERR_INVALID_ARG;
    }

    // ESP_LOGI(TAG, "✅ API Key leída y validada correctamente desde NVS");

    return ESP_OK;
}

/**
 * @brief Elimina la API Key almacenada en NVS.
 *
 * Esta función se utiliza cuando una API Key falla la validación o autenticación,
 * evitando reintentos innecesarios con credenciales incorrectas.
 *
 * La función es idempotente: si la key no existe, no genera error.
 *
 * @return ESP_OK si se eliminó correctamente o no existía
 *         Código de error ESP_ERR_* en caso de fallo
 */
esp_err_t nvs_delete_api_key(void)
{
    ESP_LOGI(TAG, "🗑️ Intentando eliminar API Key de NVS...");

    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error abriendo NVS namespace 'settings': %s", esp_err_to_name(err));
        nvs_unlock();
        return err;
    }

    // Eliminar directamente sin verificar primero (más eficiente)
    err = nvs_erase_key(nvs_handle, "openai_key");
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "✅ API Key eliminada exitosamente de NVS.");
        }
        else
        {
            ESP_LOGE(TAG, "❌ Error haciendo commit: %s", esp_err_to_name(err));
        }
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "⚠️ No se encontró API Key para eliminar.");
        err = ESP_OK; // Operación idempotente
    }
    else
    {
        ESP_LOGE(TAG, "❌ Error eliminando API Key: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    nvs_unlock();

    return err;
}

/**
 * @brief Lista todas las API Keys almacenadas en NVS.
 *        Muestra información de validación y seguridad para cada key.
 *
 * Esta función es útil para debugging y verificación de credenciales
 * almacenadas. Por seguridad, solo muestra el prefijo y sufijo de cada key.
 */
void list_api_keys_from_nvs(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "API Keys almacenadas en NVS");
    ESP_LOGI(TAG, "========================================");

    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);

    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "⚠️  No se pudo abrir namespace 'settings': %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "   (Es normal si aún no se ha guardado ninguna API Key)");
        nvs_unlock();
        return;
    }

    // Obtener el tamaño requerido para la API Key
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "openai_key", NULL, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "❌ No se encontró ninguna API Key almacenada en NVS.");
        ESP_LOGI(TAG, "   Estado: SIN CONFIGURAR");
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error al leer API Key: %s", esp_err_to_name(err));
    }
    else
    {
        // La API Key existe, leer su contenido
        char api_key_buffer[200];
        memset(api_key_buffer, 0, sizeof(api_key_buffer));

        size_t buffer_size = sizeof(api_key_buffer);
        err = nvs_get_str(nvs_handle, "openai_key", api_key_buffer, &buffer_size);

        if (err == ESP_OK)
        {
            size_t key_length = strlen(api_key_buffer);

            ESP_LOGI(TAG, "✅ API Key encontrada");
            ESP_LOGI(TAG, "   Tamaño: %zu caracteres", key_length);

            // Mostrar información de validación
            esp_err_t validation = validate_openai_api_key(api_key_buffer);
            if (validation == ESP_OK)
            {
                ESP_LOGI(TAG, "   Estado: ✅ VÁLIDA");
            }
            else
            {
                ESP_LOGI(TAG, "   Estado: ❌ INVÁLIDA");
            }

            ESP_LOGI(TAG, "   Vista previa omitida por seguridad");

            // Determinar tipo de API Key
            if (strncmp(api_key_buffer, "sk-proj-", 8) == 0)
            {
                ESP_LOGI(TAG, "   Tipo: API Key de Proyecto (sk-proj-*)");
            }
            else if (strncmp(api_key_buffer, "sk-", 3) == 0)
            {
                ESP_LOGI(TAG, "   Tipo: API Key Legacy (sk-*)");
            }
            else
            {
                ESP_LOGI(TAG, "   Tipo: DESCONOCIDO (¿formato correcto?)");
            }
        }
        else
        {
            ESP_LOGE(TAG, "❌ Error al leer API Key: %s", esp_err_to_name(err));
        }
    }

    nvs_close(nvs_handle);
    nvs_unlock();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Fin del listado de API Keys");
    ESP_LOGI(TAG, "========================================");
}

/**
 * @brief Información resumida del estado de la API Key en NVS.
 *        Función compacta para verificaciones rápidas.
 *
 * @return ESP_OK si hay una API Key válida almacenada
 *         ESP_ERR_NVS_NOT_FOUND si no hay API Key
 *         ESP_ERR_INVALID_ARG si la API Key es inválida
 */
esp_err_t get_api_key_status(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_handle);

    if (err != ESP_OK)
    {
        nvs_unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }

    char api_key_buffer[200];
    memset(api_key_buffer, 0, sizeof(api_key_buffer));
    size_t buffer_size = sizeof(api_key_buffer);

    err = nvs_get_str(nvs_handle, "openai_key", api_key_buffer, &buffer_size);
    nvs_close(nvs_handle);
    nvs_unlock();

    if (err != ESP_OK)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (api_key_buffer[0] == '\0')
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    // Validar formato
    esp_err_t validation = validate_openai_api_key(api_key_buffer);
    return validation;
}

void nvs_set_boot_to_provisioning_flag(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("system", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_set_u8(nvs_handle, "prov_flag", 1);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGW(TAG, "NVS: Bandera 'boot_to_provisioning' establecida.");
    }
}

bool nvs_read_and_clear_boot_to_provisioning_flag(void)
{
    nvs_handle_t nvs_handle;
    bool flag_is_set = false;
    esp_err_t err = nvs_open("system", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        uint8_t flag_val = 0;
        if (nvs_get_u8(nvs_handle, "prov_flag", &flag_val) == ESP_OK && flag_val == 1)
        {
            flag_is_set = true;
            ESP_LOGW(TAG, "NVS: Bandera 'boot_to_provisioning' encontrada. Borrándola...");
            nvs_erase_key(nvs_handle, "prov_flag");
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    return flag_is_set;
}

/* End of file */
