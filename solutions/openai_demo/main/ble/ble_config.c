/**
 * @file ble_config.c
 * @brief BLE WiFi Bridge module for ESP32-S3-BOX3 AI Chatbot provisioning system.
 *
 * This module implements a BLE-based WiFi provisioning system that allows users to
 * configure WiFi credentials through a BLE GATT service when the device cannot
 * connect to a pre-configured network. It provides a fallback connection method
 * for the AI chatbot system.
 *
 * Key features:
 * - Custom GATT service for WiFi credential and OpenAI API key configuration
 * - Secure handling and validation of received credentials
 * - WiFi/BLE coexistence configuration for optimal performance
 * - Automatic advertising management (start/stop based on WiFi status)
 * - Special command support for NVS erasure and device reset
 * - Input validation and error handling for received credentials
 *
 * The system uses NimBLE stack (lightweight BLE implementation) and integrates
 * with the main application's network management system.
 *
 * @note This project is based on the ESP WebRTC solution from Espressif:
 *       https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/openai_demo
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

// Includes del sistema estándar
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Includes de ESP-IDF Core
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"

// Includes de Bluetooth/BLE Stack
#include "esp_nimble_hci.h"

// Includes de NimBLE Stack
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"
#include "os/os_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Includes de FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Includes del proyecto
#include "network.h"
#include "ble_config.h"
#include "nvs_setup.h"
#include "ui.h"
#include "ble_common.h"
#include "network_storage.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define BLE_PREFERRED_MTU 247

/* Private function declarations */
// inline static void format_addr(char *addr_str, uint8_t addr[]);

static const char *TAG = "BLE_CONFIG";
static bool g_provisioning_active = true; // Controla si el modo provisioning está activo

/**
 * @brief Custom 128-bit UUID for the WiFi provisioning service.
 *
 * Uses a custom UUID to avoid conflicts with standard BLE services.
 * This UUID uniquely identifies our WiFi configuration service.
 */
static const ble_uuid128_t wifi_service_uuid =
    BLE_UUID128_INIT(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xff,
                     0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0x00);

/**
 * @brief Custom 128-bit UUID for the WiFi credential characteristic.
 *
 * This characteristic handles WiFi credential write operations from BLE clients.
 * Supports both standard write and write-without-response operations.
 */
static const ble_uuid128_t wifi_char_uuid =
    BLE_UUID128_INIT(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xff,
                     0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xff, 0x01);

/**
 * @brief Custom 128-bit UUID for the OpenAI API Key characteristic.
 *
 * This characteristic is used to write the user's OpenAI API Key.
 */
static const ble_uuid128_t api_key_char_uuid =
    BLE_UUID128_INIT(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xff,
                     0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xff, 0x02);

/**
 * @brief Custom 128-bit UUID for the System Command characteristic.
 *
 * This characteristic is used to send special commands like ERASE_NVS.
 */
static const ble_uuid128_t sys_cmd_char_uuid =
    BLE_UUID128_INIT(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xff,
                     0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xff, 0x03);

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

// Declaraciones de funciones estáticas
static const char *ble_error_to_string(int error_code);
static int copy_gatt_payload_to_string(struct os_mbuf *om, char *dst, size_t dst_size,
                                       size_t *out_len, const char *label);
static char *trim_ascii_whitespace(char *text);
static void ble_app_advertise(void);
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static esp_err_t validate_wifi_credentials(const char *ssid, const char *password);
// Handlers específicos para cada característica
static int gatt_svr_chr_write_wifi_creds(struct os_mbuf *om);
static int gatt_svr_chr_write_api_key(struct os_mbuf *om);
static int gatt_svr_chr_write_sys_cmd(struct os_mbuf *om);

/**
 * @brief Converts BLE error codes to human-readable strings.
 *
 * This helper function provides meaningful error descriptions for debugging
 * and logging purposes, making it easier to diagnose BLE-related issues.
 *
 * @param error_code BLE error code from NimBLE operations
 * @return Pointer to static string describing the error
 */
static const char *ble_error_to_string(int error_code)
{
    switch (error_code)
    {
    case 0:
        return "Success";
    case BLE_HS_EAGAIN:
        return "Temporary failure";
    case BLE_HS_EALREADY:
        return "Operation already in progress";
    case BLE_HS_EINVAL:
        return "Invalid argument";
    case BLE_HS_EMSGSIZE:
        return "Message size error";
    case BLE_HS_ENOENT:
        return "Entry not found";
    case BLE_HS_ENOMEM:
        return "Out of memory";
    case BLE_HS_ENOTCONN:
        return "Not connected";
    case BLE_HS_ENOTSUP:
        return "Operation not supported";
    case BLE_HS_EAPP:
        return "Application error";
    case BLE_HS_EBADDATA:
        return "Bad data";
    case BLE_HS_EOS:
        return "Operating system error";
    case BLE_HS_ECONTROLLER:
        return "Controller error";
    case BLE_HS_ETIMEOUT:
        return "Timeout";
    case BLE_HS_EDONE:
        return "Operation complete";
    case BLE_HS_EBUSY:
        return "Resource busy";
    default:
        return "Unknown error";
    }
}

static int copy_gatt_payload_to_string(struct os_mbuf *om, char *dst, size_t dst_size,
                                       size_t *out_len, const char *label)
{
    if (!om || !dst || dst_size == 0)
    {
        ESP_LOGW(TAG, "%s vacío recibido vía BLE", label);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t total_len = OS_MBUF_PKTLEN(om);
    if (total_len == 0)
    {
        ESP_LOGW(TAG, "%s vacío recibido vía BLE", label);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (total_len >= dst_size)
    {
        ESP_LOGE(TAG, "%s demasiado largo: %u bytes, máximo %u",
                 label, (unsigned)total_len, (unsigned)(dst_size - 1));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    int rc = os_mbuf_copydata(om, 0, total_len, dst);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "No se pudo copiar %s desde mbuf BLE: rc=%d", label, rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    dst[total_len] = '\0';
    if (out_len)
    {
        *out_len = total_len;
    }

    ESP_LOGI(TAG, "%s recibido (%u bytes total, primer mbuf=%u)",
             label, (unsigned)total_len, (unsigned)om->om_len);
    return 0;
}

static char *trim_ascii_whitespace(char *text)
{
    if (!text)
    {
        return NULL;
    }

    while (*text && isspace((unsigned char)*text))
    {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
    {
        *--end = '\0';
    }

    return text;
}

/**
 * @brief Handler for system commands received via BLE.
 *       Currently supports "CMD:ERASE_NVS" to erase NVS and restart the device.
 *      Additional commands can be added as needed.
 * @param om Pointer to the os_mbuf containing the received command data
 * @return 0 on success, BLE_ATT_ERR_* code on failure
 */
static int gatt_svr_chr_write_sys_cmd(struct os_mbuf *om)
{
    char received_data[32] = {0};
    int rc = copy_gatt_payload_to_string(om, received_data, sizeof(received_data), NULL, "Comando de sistema");
    if (rc != 0)
    {
        return rc;
    }

    ESP_LOGW(TAG, "Comando de sistema recibido: '%s'", received_data);

    if (strcmp(received_data, "CMD:ERASE_NVS") == 0)
    {
        ESP_LOGW(TAG, "Ejecutando comando: Borrar NVS y reiniciar...");
        display_resetting_message();     // Muestra el mensaje "Resetting..."
        vTaskDelay(pdMS_TO_TICKS(2000)); // Pausa para ACK de BLE y que se vea el mensaje
        erase_nvs();
        return 0;
    }

    // Aquí podrías añadir más comandos en el futuro con 'else if'

    ESP_LOGE(TAG, "Comando de sistema no reconocido: '%s'", received_data);
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
}

/**
 * @brief Handler for WiFi credentials received via BLE.
 *
 * Parses and validates the received SSID and password, saves them to NVS,
 * and then restarts the device to apply the new configuration.
 *
 * Expected format: "SSID PASSWORD"
 *
 * @param om Pointer to the os_mbuf containing the received credential data
 * @return 0 on success, BLE_ATT_ERR_* code on failure
 */
static int gatt_svr_chr_write_wifi_creds(struct os_mbuf *om)
{
    // Buffer seguro para los datos recibidos + null terminator
    char received_data[BLE_MAX_RECEIVED_DATA_LEN] = {0};
    size_t received_len = 0;
    int rc = copy_gatt_payload_to_string(om, received_data, sizeof(received_data), &received_len, "Datos WiFi");
    if (rc != 0)
    {
        return rc;
    }

    ESP_LOGI(TAG, "Datos WiFi aplanados (%zu bytes)", received_len);

    // Limpiar caracteres de control (CR/LF)
    for (int i = strlen(received_data) - 1; i >= 0; i--)
    {
        if (received_data[i] == '\r' || received_data[i] == '\n' || received_data[i] == '\0')
        {
            received_data[i] = '\0';
        }
        else
        {
            break;
        }
    }

    // Parsear credenciales WiFi (formato: "SSID PASSWORD")
    char *separator = strchr(received_data, ' ');
    if (!separator)
    {
        ESP_LOGW(TAG, "Formato inválido - se esperaba formato 'SSID PASSWORD'");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    *separator = '\0';
    char *ssid = received_data;
    char *password = separator + 1;

    // Validar credenciales (longitud, etc.)
    if (validate_wifi_credentials(ssid, password) != ESP_OK)
    {
        ESP_LOGE(TAG, "Credenciales WiFi inválidas");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    ESP_LOGI(TAG, "=== CREDENCIALES RECIBIDAS (BLE) ===");
    ESP_LOGI(TAG, "SSID: '%s'", ssid);
    ESP_LOGI(TAG, "Password: '[%zu caracteres]'", strlen(password));
    ESP_LOGI(TAG, "===================================");

    // 1. Guardar las credenciales usando la función existente (que es void).
    esp_err_t save_err = network_save_wifi_credentials(ssid, password);
    if (save_err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudieron guardar credenciales WiFi en NVS: %s",
                 esp_err_to_name(save_err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 2. Como la función es void, asumimos que funcionó si no hubo un crash.
    //    Procedemos directamente a reiniciar.
    ESP_LOGI(TAG, "Credenciales WiFi guardadas en network_storage. Reiniciando dispositivo...");
    display_resetting_message();     // Muestra "Resetting..."
    vTaskDelay(pdMS_TO_TICKS(2000)); // Pausa para ACK de BLE y que se vea el mensaje
    esp_restart();

    // Este return nunca se alcanzará debido al esp_restart()
    return 0;
}

/**
 * @brief Handler for OpenAI API Key received via BLE.
 *
 * Validates and stores the received API key, then restarts the device
 * to apply the new configuration.
 *
 * @param om Pointer to the os_mbuf containing the received API key data
 * @return 0 on success, BLE_ATT_ERR_* code on failure
 */
static int gatt_svr_chr_write_api_key(struct os_mbuf *om)
{
    // Buffer seguro para la API Key
    char api_key[BLE_MAX_RECEIVED_DATA_LEN] = {0};
    size_t api_key_len = 0;
    int rc = copy_gatt_payload_to_string(om, api_key, sizeof(api_key), &api_key_len, "API Key");
    if (rc != 0)
    {
        return rc;
    }

    char *clean_api_key = trim_ascii_whitespace(api_key);
    ESP_LOGI(TAG, "API Key recibida ([%zu] bytes BLE, %zu caracteres útiles)",
             api_key_len, strlen(clean_api_key));
    if (strncmp(clean_api_key, "sk-proj-", 8) == 0 && strlen(clean_api_key) < 160)
    {
        ESP_LOGE(TAG, "API Key de proyecto truncada antes de guardar (%zu caracteres).",
                 strlen(clean_api_key));
        ESP_LOGE(TAG, "Revisa MTU BLE o envía la API Key con write-with-response/chunks desde la app.");
    }

    esp_err_t ret = nvs_save_api_key(clean_api_key);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "API Key guardada en NVS. Reiniciando dispositivo...");
        display_resetting_message();     // Muestra "Trying to connect..."
        vTaskDelay(pdMS_TO_TICKS(2000)); // Pequeña pausa para que el cliente BLE reciba la confirmación
        esp_restart();
    }
    else
    {
        ESP_LOGE(TAG, "Fallo al guardar la API Key en NVS.");
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}

/**
 * @brief Validates WiFi credentials received via BLE.
 *
 * Performs comprehensive validation of SSID and password parameters:
 * - Checks for null pointers and empty strings
 * - Validates length constraints to prevent buffer overflows
 * - Ensures compatibility with WiFi standards
 *
 * @param ssid WiFi network name to validate
 * @param password WiFi network password to validate
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG if invalid
 */
static esp_err_t validate_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0)
    {
        ESP_LOGW(TAG, "SSID vacío");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(ssid) > BLE_WIFI_MAX_SSID_LEN)
    {
        ESP_LOGW(TAG, "SSID demasiado largo (máximo %d caracteres)", BLE_WIFI_MAX_SSID_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (!password)
    {
        ESP_LOGW(TAG, "Password nulo");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(password) > BLE_WIFI_MAX_PASSWORD_LEN)
    {
        ESP_LOGW(TAG, "Password demasiado largo (máximo %d caracteres)", BLE_WIFI_MAX_PASSWORD_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief BLE GAP event handler for connection management.
 *
 * Handles BLE Generic Access Profile events including:
 * - Connection establishment and termination
 * - Advertising completion events
 * - Connection parameter updates
 *
 * This handler manages the BLE connection lifecycle and automatically
 * restarts advertising when appropriate.
 *
 * @param event Pointer to BLE GAP event structure
 * @param arg User argument (unused)
 * @return 0 on success
 */
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Cliente conectado");
        if (event->connect.status == 0)
        {
            conn_handle = event->connect.conn_handle;
        }
        else
        {
            ESP_LOGE(TAG, "Error en conexión: %d", event->connect.status);
            // Solo reiniciar si el provisioning sigue activo
            if (g_provisioning_active)
            {
                ble_app_advertise();
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Cliente desconectado.");
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        // Solo reiniciar si el provisioning sigue activo
        if (g_provisioning_active)
        {
            ble_app_advertise();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising completado");
        // No reiniciar advertising automáticamente para ahorrar energía
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU BLE negociado: conn=%d cid=%d mtu=%d",
                 event->mtu.conn_handle,
                 event->mtu.channel_id,
                 event->mtu.value);
        break;

    default:
        break;
    }

    return 0;
}

/**
 * @brief GATT access callback for WiFi credential characteristic.
 *
 * This is the core function that handles WiFi credential exchange via BLE.
 * It processes write operations to the WiFi characteristic and:
 *
 * 1. Validates incoming data format and length
 * 2. Parses SSID and password from received data
 * 3. Handles special commands (e.g., NVS erase)
 * 4. Initiates WiFi connection with provided credentials
 * 5. Manages BLE advertising based on connection results
 *
 * Expected data format: "SSID PASSWORD" (space-separated)
 * Special commands: "CMD:ERASE_NVS" (triggers device reset)
 *
 * @param conn_handle BLE connection handle
 * @param attr_handle GATT attribute handle
 * @param ctxt GATT access context containing operation details and data
 * @param arg User argument (unused)
 * @return BLE error code (0 for success)
 */
static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &wifi_char_uuid.u) == 0)
    {
        return gatt_svr_chr_write_wifi_creds(ctxt->om);
    }
    if (ble_uuid_cmp(uuid, &api_key_char_uuid.u) == 0)
    {
        return gatt_svr_chr_write_api_key(ctxt->om);
    }
    if (ble_uuid_cmp(uuid, &sys_cmd_char_uuid.u) == 0)
    {
        return gatt_svr_chr_write_sys_cmd(ctxt->om);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief GATT service definition for WiFi provisioning.
 *
 * Defines the custom GATT service structure with:
 * - Primary service with custom 128-bit UUID
 * - Single characteristic for WiFi credential exchange
 * - Write and write-without-response permissions
 * - Custom access callback for credential processing
 */
static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &wifi_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* Característica de WiFi */
                .uuid = &wifi_char_uuid.u,
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* Característica de API Key */
                .uuid = &api_key_char_uuid.u,
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* NUEVA Característica de Comandos */
                .uuid = &sys_cmd_char_uuid.u,
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0}, // Terminador
        },
    },
    {0}, // Terminador
};

/**
 * @brief Starts BLE advertising for WiFi provisioning discovery.
 *
 * Configures and starts BLE advertising with:
 * - General discoverable and connectable mode
 * - Device name in advertising data
 * - Optimized advertising intervals (30-60ms)
 * - Indefinite advertising duration
 *
 * This makes the device discoverable by BLE clients for WiFi provisioning.
 */
static void ble_app_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *device_name;
    int rc;

    ESP_LOGI(TAG, "Iniciando advertising BLE...");

    // Limpiar estructuras para evitar datos basura
    memset(&adv_params, 0, sizeof(adv_params));
    memset(&fields, 0, sizeof(fields));

    static const uint8_t mfg_marker[] = {'A', 'I', 'C'}; // "AIC" marker
    // Configurar campos de advertising
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Obtener y configurar nombre del dispositivo
    device_name = ble_svc_gap_device_name();
    if (!device_name)
    {
        ESP_LOGE(TAG, "Error obteniendo nombre del dispositivo");
        return;
    }

    /* Nombre */
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    /* Manufacturer data (muy fiable para filtrar en la app) */
    fields.mfg_data = (uint8_t *)mfg_marker;
    fields.mfg_data_len = sizeof(mfg_marker);

    // Aplicar campos de advertising
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error configurando campos de advertising: %d (%s)",
                 rc, ble_error_to_string(rc));
        return;
    }

    // Configurar parámetros de advertising optimizados
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // Connectable undirected
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // General discoverable
    adv_params.itvl_min = 0x30;                   // 30ms - balance entre consumo y detectabilidad
    adv_params.itvl_max = 0x60;                   // 60ms

    // Iniciar advertising indefinido
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error iniciando advertising: %d (%s)",
                 rc, ble_error_to_string(rc));
        return;
    }

    ESP_LOGI(TAG, "Advertising BLE iniciado exitosamente - Dispositivo: '%s'", device_name);
}

esp_err_t ble_wifi_register_services(void)
{
    ESP_LOGI(TAG, "Registrando servicios GATT para Provisioning...");

    int rc;
    // Configurar nombre del dispositivo
    rc = ble_svc_gap_device_name_set(BLE_WIFI_DEVICE_NAME);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error configurando nombre: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_att_set_preferred_mtu(BLE_PREFERRED_MTU);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "No se pudo configurar MTU BLE preferido %d: %d (%s)",
                 BLE_PREFERRED_MTU, rc, ble_error_to_string(rc));
    }
    else
    {
        ESP_LOGI(TAG, "MTU BLE preferido configurado: %d", BLE_PREFERRED_MTU);
    }

    // Registrar servicios GATT
    rc = ble_gatts_count_cfg(gatt_services);
    rc |= ble_gatts_add_svcs(gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error registrando servicios GATT: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Servicios de Provisioning registrados en la configuración del host.");
    return ESP_OK;
}

void ble_wifi_start_advertising(void)
{
    g_provisioning_active = true;
    if (!ble_hs_synced())
    {
        ESP_LOGE(TAG, "No se puede iniciar advertising, el stack no está sincronizado.");
        return;
    }
    ble_app_advertise();
}

void ble_wifi_provisioning_stop(void)
{
    ESP_LOGI(TAG, "Deteniendo advertising de Provisioning WiFi...");
    if (!ble_hs_synced())
    {
        ESP_LOGI(TAG, "Advertising BLE no estaba activo: host no sincronizado.");
        return;
    }
    if (!ble_gap_adv_active())
    {
        ESP_LOGI(TAG, "Advertising BLE no estaba activo.");
        return;
    }
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "Error deteniendo advertising: %d", rc);
    }
}

/**
 * @brief Desactiva permanentemente el modo de aprovisionamiento.
 * * Esta función baja la bandera de control para que el manejador de eventos
 * deje de reiniciar el advertising, y detiene cualquier advertising en curso.
 */
void ble_wifi_provisioning_deinit(void)
{
    ESP_LOGI(TAG, "Desactivando permanentemente el modo Provisioning.");
    g_provisioning_active = false;
    ble_wifi_provisioning_stop(); // Llama a tu función para detener el advertising
}
