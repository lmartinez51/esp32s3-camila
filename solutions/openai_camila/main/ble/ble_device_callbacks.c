/**
 * @file ble_device_callbacks.c
 * @brief Implementación de callbacks para control de dispositivos BLE
 */

#include "ble_device_callbacks.h"
#include "esp_log.h"
#include "ui.h" // Para mostrar notificaciones en pantalla si es necesario

static const char *TAG = "BLE_CALLBACKS";

/* Variables privadas para estadísticas */
static int discovered_devices_count = 0;
static int connected_devices_count = 0;
static int successful_commands_count = 0;
static int failed_commands_count = 0;

/**
 * @brief Convierte tipo de dispositivo a string para logging
 */
static const char *device_type_to_string(ble_device_type_t type)
{
    switch (type)
    {
    case BLE_DEVICE_TYPE_LIGHT:
        return "Foco/Luz";
    case BLE_DEVICE_TYPE_FAN:
        return "Ventilador";
    case BLE_DEVICE_TYPE_VACUUM:
        return "Aspiradora";
    case BLE_DEVICE_TYPE_SPEAKER:
        return "Bocina";
    case BLE_DEVICE_TYPE_THERMOSTAT:
        return "Termostato";
    case BLE_DEVICE_TYPE_CUSTOM:
        return "Personalizado";
    default:
        return "Desconocido";
    }
}

/**
 * @brief Callback llamado cuando se descubre un dispositivo BLE
 */
// Reemplazar la función existente con esta versión mejorada:

void on_device_discovered_callback(ble_device_info_t *device)
{
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo NULL en callback de descubrimiento");
        return;
    }

    int current_total = ble_device_get_discovered_count();

    ESP_LOGI(TAG, "=== DISPOSITIVO BLE DESCUBIERTO ===");
    ESP_LOGI(TAG, "Nombre: %s", device->name);
    ESP_LOGI(TAG, "Tipo: %s", device_type_to_string(device->type));
    ESP_LOGI(TAG, "RSSI: %d dBm", device->rssi);
    ESP_LOGI(TAG, "Dirección: %02X:%02X:%02X:%02X:%02X:%02X",
             device->addr[0], device->addr[1], device->addr[2],
             device->addr[3], device->addr[4], device->addr[5]);
    ESP_LOGI(TAG, "Total descubiertos: %d", current_total); // discovered_devices_count); // Cambiado a total_discovered_for_callback
    ESP_LOGI(TAG, "================================");

    // Lógica específica por tipo de dispositivo
    switch (device->type)
    {
    case BLE_DEVICE_TYPE_LIGHT:
        ESP_LOGI(TAG, "💡 Foco inteligente detectado: %s", device->name);
        break;
    case BLE_DEVICE_TYPE_FAN:
        ESP_LOGI(TAG, "🌪️ Ventilador inteligente detectado: %s", device->name);
        break;
    case BLE_DEVICE_TYPE_SPEAKER:
        ESP_LOGI(TAG, "🔊 Bocina inteligente detectada: %s", device->name);
        break;
    default:
        ESP_LOGI(TAG, "🔍 Dispositivo genérico detectado: %s", device->name);
        break;
    }
}

/**
 * @brief Callback llamado cuando se conecta un dispositivo BLE
 */
void on_device_connected_callback(ble_device_info_t *device)
{
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo NULL en callback de conexión");
        return;
    }

    connected_devices_count++;

    ESP_LOGI(TAG, "=== DISPOSITIVO CONECTADO ===");
    ESP_LOGI(TAG, "✅ Conexión establecida con: %s", device->name);
    ESP_LOGI(TAG, "Tipo: %s", device_type_to_string(device->type));
    ESP_LOGI(TAG, "Connection Handle: %d", device->conn_handle);
    ESP_LOGI(TAG, "Dispositivos conectados: %d", connected_devices_count);
    ESP_LOGI(TAG, "============================");

    // Notificar al sistema AI que el dispositivo está disponible
    // Aquí podrías enviar un evento al sistema de reconocimiento de voz
    // para que sepa que hay un nuevo dispositivo controlable

    // Ejemplo: Mostrar notificación en pantalla (si tienes UI)
    /*
    char notification[64];
    snprintf(notification, sizeof(notification),
             "Dispositivo conectado: %s", device->name);
    ui_show_notification(notification, 3000); // Mostrar por 3 segundos
    */

    // Al final de la función, agregar:
#if AUTO_CONNECT_ENABLED
    // Verificar aqui si debe enviar comando de prueba

#endif
}

/**
 * @brief Callback llamado cuando se desconecta un dispositivo BLE
 */
void on_device_disconnected_callback(ble_device_info_t *device)
{
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo NULL en callback de desconexión");
        return;
    }

    if (connected_devices_count > 0)
    {
        connected_devices_count--;
    }

    ESP_LOGW(TAG, "=== DISPOSITIVO DESCONECTADO ===");
    ESP_LOGW(TAG, "❌ Conexión perdida con: %s", device->name);
    ESP_LOGW(TAG, "Tipo: %s", device_type_to_string(device->type));
    ESP_LOGW(TAG, "Dispositivos conectados: %d", connected_devices_count);
    ESP_LOGW(TAG, "==============================");

    // Resetear estado y handle
    device->state = BLE_DEVICE_STATE_DISCONNECTED;
    device->conn_handle = BLE_HS_CONN_HANDLE_NONE;

    // Aquí podrías marcar flags adicionales o planear reintentos
}

/**
 * @brief Callback llamado con el resultado de un comando enviado
 */
void on_command_result_callback(ble_device_info_t *device, bool success)
{
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo NULL en callback de resultado");
        return;
    }

    if (success)
    {
        successful_commands_count++;
        ESP_LOGI(TAG, "=== COMANDO EXITOSO ===");
        ESP_LOGI(TAG, "✅ Comando enviado correctamente a: %s", device->name);
        ESP_LOGI(TAG, "Comandos exitosos: %d", successful_commands_count);
        ESP_LOGI(TAG, "=====================");
    }
    else
    {
        failed_commands_count++;
        ESP_LOGE(TAG, "=== COMANDO FALLIDO ===");
        ESP_LOGE(TAG, "❌ Error enviando comando a: %s", device->name);
        ESP_LOGE(TAG, "Comandos fallidos: %d", failed_commands_count);
        ESP_LOGE(TAG, "=====================");
    }

    // Notificar al sistema AI sobre el resultado
    // Esto es importante para que el chatbot pueda responder adecuadamente
    // al usuario sobre si el comando se ejecutó correctamente

    // Ejemplo: Enviar respuesta de voz
    /*
    if (success) {
        // Aquí podrías hacer que el sistema diga algo como:
        // "Comando ejecutado correctamente en [nombre del dispositivo]"
    } else {
        // "Error ejecutando comando en [nombre del dispositivo]"
    }
    */
}

/**
 * @brief Estructura de callbacks configurada para la aplicación
 */
static ble_device_callbacks_t app_callbacks = {
    .on_device_discovered = on_device_discovered_callback,
    .on_device_connected = on_device_connected_callback,
    .on_device_disconnected = on_device_disconnected_callback,
    .on_command_result = on_command_result_callback,
    .on_discovery_stopped = on_discovery_stopped_callback};

/**
 * @brief Obtiene la estructura de callbacks configurada para la aplicación
 */
ble_device_callbacks_t *ble_device_get_app_callbacks(void)
{
    return &app_callbacks;
}

/**
 * @brief Obtiene estadísticas de los callbacks (función de utilidad)
 */
void ble_callbacks_get_stats(int *discovered, int *connected, int *success_cmd, int *failed_cmd)
{
    if (discovered)
        *discovered = discovered_devices_count;
    if (connected)
        *connected = connected_devices_count;
    if (success_cmd)
        *success_cmd = successful_commands_count;
    if (failed_cmd)
        *failed_cmd = failed_commands_count;
}

/**
 * @brief Resetea las estadísticas de callbacks
 */
void ble_callbacks_reset_stats(void)
{
    discovered_devices_count = 0;
    connected_devices_count = 0;
    successful_commands_count = 0;
    failed_commands_count = 0;
    ESP_LOGI(TAG, "Estadísticas de callbacks reseteadas");
}

void on_discovery_stopped_callback(int nuevos_dispositivos)
{
    if (nuevos_dispositivos > 0)
    {
        ESP_LOGI("BLE_CALLBACKS", "🔎 Descubrimiento finalizado. %d dispositivos nuevos encontrados.", nuevos_dispositivos);
    }
    else
    {
        ESP_LOGI("BLE_CALLBACKS", "🔎 Descubrimiento finalizado. No se encontraron dispositivos nuevos.");
    }
}
