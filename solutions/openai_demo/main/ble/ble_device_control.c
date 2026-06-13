/**
 * @file ble_device_control.c
 * @brief BLE Device Control implementation for ESP32-S3-BOX3 AI Chatbot (ESP-IDF 5.4 Adapted)
 *
 * This module implements BLE device discovery, connection management,
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// NimBLE stack (ESP-IDF 5.4)
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#include "ble_device_control.h"
#include "ble_config.h"
#include "ble_common.h" // módulo común BLE de tu proyecto
#include "nvs_setup.h"
#include "wifi_session_state.h"
#include "ble_device_callbacks.h"
#include "app_events.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define DISCOVERY_TIMEOUT_MS 10000
#define AUTO_CONNECT_ENABLED 0
#define AUTO_CONNECT_MIN_RSSI -75        // Solo conectar si señal es buena
#define AUTO_CONNECT_MAX_SIMULTANEOUS 2  // Máximo 2 conexiones simultáneas
#define AUTO_CONNECT_DELAY_MS 1000       // Esperar 1s entre conexiones
#define TEST_COMMAND_DELAY_MS 3000       // Esperar 3s después de conectar para enviar comando
#define BLE_COMMAND_TIMEOUT_MS 1000      // 1 segundos timeout por comando
#define MAX_KNOWN_PROFILES 10            // Máximo perfiles conocidos en memoria
#define BLE_CENTRAL_DIAGNOSTIC_ENABLED 1 // Temporal: Habilitar logs detallados para diagnóstico de BLE Central

// Telemetría: ops activas
#define INCR_ACTIVE_OPS()                                                 \
    do                                                                    \
    {                                                                     \
        int old = atomic_fetch_add(&g_active_ble_operations, 1);          \
        ESP_LOGD(TAG, "g_active_ble_operations: %d -> %d", old, old + 1); \
    } while (0)

#define DECR_ACTIVE_OPS()                                                 \
    do                                                                    \
    {                                                                     \
        int old = atomic_fetch_sub(&g_active_ble_operations, 1);          \
        ESP_LOGD(TAG, "g_active_ble_operations: %d -> %d", old, old - 1); \
    } while (0)

/**
 * @brief Número de ciclos de descubrimiento consecutivos sin encontrar
 * dispositivos nuevos antes de que la tarea se detenga sola.
 */
#define MAX_EMPTY_CYCLES 2

/**
 * @brief Tiempo de inactividad (en milisegundos) para detener la tarea si no se
 * han encontrado nuevos dispositivos. Se usa un umbral más largo la
 * primera vez que se explora una red (cuando no hay dispositivos conocidos).
 */
#define IDLE_MS_FIRST_VISIT 180000 // 3 minutos

/**
 * @brief Tiempo de inactividad (en milisegundos) para detener la tarea.
 * Se usa un umbral más corto en visitas subsecuentes a una red
 * que ya conocemos.
 */
#define IDLE_MS_SUBSEQUENT 90000 // 1.5 minutos

/**
 * @brief Umbral mínimo de memoria heap libre. Si la memoria del sistema
 * cae por debajo de este valor, la tarea de descubrimiento se
 * detendrá para liberar recursos.
 */
#define MIN_HEAP_BYTES (30 * 1024) // 30 KB
#define BLE_CONTROL_QUEUE_LEN 8
#define BLE_CONTROL_QUEUE_ITEM_SIZE sizeof(ble_control_op_t)
#define BLE_CONTROL_WORKER_STOP_TIMEOUT_MS 3000
#define BLE_SMART_TASK_STOP_TIMEOUT_MS 12000

#define BLE_CONTROL_WORKER_STACK_BYTES 4096
#define BLE_SMART_TASK_STACK_BYTES 4096
#define BLE_CONTROL_WORKER_PRIORITY (tskIDLE_PRIORITY + 3)
#define BLE_SMART_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define BLE_RUNTIME_TASK_CORE 0
#define BLE_RUNTIME_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BLE_STAGING_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define BLE_CENTRAL_MIN_INTERNAL_FREE_BYTES (4 * 1024)
#define BLE_CENTRAL_MIN_INTERNAL_LARGEST_BLOCK_BYTES (2 * 1024)
#define IDENTITY_EARLY_EXIT_CANCEL_WAIT_MS 1000

// Variables globales
static const char *TAG = "BLE_DEVICE_CTRL";

static bool ble_central_diagnostic_enabled(void)
{
    return BLE_CENTRAL_DIAGNOSTIC_ENABLED != 0;
}

static void ble_log_memory_snapshot(const char *stage)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGD(TAG,
             "[BLE_MEM] %s | Internal: Free=%zu Largest=%zu Min=%zu | PSRAM: Free=%zu Largest=%zu",
             stage ? stage : "(null)",
             free_internal,
             largest_internal,
             min_internal,
             free_psram,
             largest_psram);
}

static esp_err_t ble_require_internal_memory_floor(const char *stage)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (free_internal < BLE_CENTRAL_MIN_INTERNAL_FREE_BYTES ||
        largest_internal < BLE_CENTRAL_MIN_INTERNAL_LARGEST_BLOCK_BYTES)
    {
        ESP_LOGE(TAG,
                 "[BLE_MEM] %s rejected: Internal Free=%zu Largest=%zu required Free>=%u Largest>=%u",
                 stage ? stage : "(null)",
                 free_internal,
                 largest_internal,
                 (unsigned)BLE_CENTRAL_MIN_INTERNAL_FREE_BYTES,
                 (unsigned)BLE_CENTRAL_MIN_INTERNAL_LARGEST_BLOCK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static bool auto_connection_globally_enabled = AUTO_CONNECT_ENABLED;
static bool nvs_loading_complete = false;
// Control de la tarea inteligente
static bool smart_discovery_enabled = false;
static bool smart_discovery_running = false;
static TaskHandle_t smart_ble_task_handle = NULL;
static SemaphoreHandle_t smart_task_start_signal = NULL;
static SemaphoreHandle_t smart_task_idle_signal = NULL;
static SemaphoreHandle_t identity_validation_mutex = NULL;
static bool module_stopping = false;
static bool ble_runtime_resources_ready = false;
static ble_discovery_metrics_t discovery_metrics;
// Base de datos de perfiles conocidos
static known_device_profile_t g_known_profiles[MAX_KNOWN_PROFILES];
static int g_known_profiles_count = 0;
static atomic_int g_active_ble_operations = 0;

static QueueHandle_t ble_control_queue = NULL;
static TaskHandle_t ble_control_worker_handle = NULL;

typedef enum
{
    BLE_SMART_TASK_MODE_DISCOVERY = 0,
    BLE_SMART_TASK_MODE_IDENTITY_VALIDATION,
} ble_smart_task_mode_t;

typedef struct
{
    uint32_t timeout_ms;
    bool seen_uuid;
    bool present;
    bool cancel_requested;
    int8_t best_rssi;
    int8_t last_rssi;
    char validated_name[32];
} ble_identity_validation_state_t;

static ble_smart_task_mode_t smart_task_mode = BLE_SMART_TASK_MODE_DISCOVERY;
static ble_identity_validation_state_t identity_validation_state = {
    .timeout_ms = 0,
    .seen_uuid = false,
    .present = false,
    .cancel_requested = false,
    .best_rssi = -127,
    .last_rssi = -127,
    .validated_name = "",
};

static const ble_uuid128_t s_identity_service_uuid = {
    .u = {
        .type = BLE_UUID_TYPE_128,
    },
    .value = {
        BLE_IDENTITY_SERVICE_UUID_BYTES,
    },
};

typedef enum
{
    BLE_OP_DISCONNECT,
    BLE_OP_STOP_SCAN,
    BLE_OP_MARK_ERROR_DISCONNECT,
    BLE_OP_SHUTDOWN,
    // Puedes añadir: BLE_OP_SAVE_NVS, BLE_OP_DELETE_NVS, etc.
} ble_control_op_type_t;

static struct
{
    uint8_t addr[6];
    uint8_t addr_type;
    bool is_active;
} g_pending_connection;

typedef struct
{
    ble_control_op_type_t type;
    uint8_t addr[6]; // usado para DISCONNECT
    // campos adicionales si agregas otras ops
} ble_control_op_t;

// Definición de discovery_context_t (se usará para timeouts de descubrimiento)
typedef struct
{
    uint16_t conn_handle;
    uint32_t start_time;
    bool services_discovered;
    bool characteristics_discovered;
    bool in_use; // Si está en uso (conectado o en descubrimiento)
} discovery_context_t;

// 2. ESTRUCTURA PARA DISPOSITIVOS DE INTERÉS
typedef struct
{
    const char *name_pattern;        // Patrón de nombre a buscar
    ble_device_type_t expected_type; // Tipo esperado
    bool auto_connect;               // Si debe conectarse automáticamente
    bool send_test_command;          // Si debe enviar comando de prueba
    int min_rssi;                    // RSSI mínimo para considerar conexión
} device_interest_t;

// En la sección de variables globales de ble_device_control.c
static struct
{
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool found;
} best_candidate;

/* Private variables */
static bool module_initialized = false;
static bool scanning_active = false;
static ble_device_callbacks_t device_callbacks = {0};
static ble_device_info_t discovered_devices[BLE_DEVICE_MAX_DEVICES];
static int discovered_count = 0;
static SemaphoreHandle_t devices_mutex = NULL;
static discovery_context_t discovery_contexts[BLE_DEVICE_MAX_DEVICES];
static int active_discoveries = 0;
static SemaphoreHandle_t scan_complete_semaphore = NULL;
static SemaphoreHandle_t ble_control_worker_stop_signal = NULL;
static bool smart_task_shutdown_requested = false;

/* Private function declarations */
static int ble_gap_scan_event_handler(struct ble_gap_event *event, void *arg);
static int ble_gap_identity_validation_event_handler(struct ble_gap_event *event, void *arg);
static int ble_gap_connect_event_handler(struct ble_gap_event *event, void *arg);
static esp_err_t add_or_update_discovered_device(ble_device_info_t *device);
static ble_device_info_t *find_device_by_addr_internal(uint8_t addr[6]);
static ble_device_info_t *find_device_by_conn_handle(uint16_t conn_handle);
static void update_device_state(uint8_t addr[6], ble_device_state_t new_state);
static int on_service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg);
static int on_characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg);
static void cleanup_discovery_context(uint16_t conn_handle);
static discovery_context_t *get_free_discovery_context(uint16_t conn_handle);
static int count_known_onboarded_devices(void);
static bool target_devices_reached(void);
static void attempt_device_reconnection(void);
static void smart_ble_discovery_btdevices_task(void *param);
static void ble_device_run_identity_validation(void);
static esp_err_t ble_device_start_identity_scan(uint32_t timeout_ms);
static bool ble_identity_adv_has_target_uuid(const struct ble_hs_adv_fields *fields);
static void ble_identity_validation_reset(uint32_t timeout_ms);
static bool ble_identity_validation_record_match(int8_t rssi, const char* name);
static bool ble_identity_validation_is_present(int8_t *best_rssi, int8_t *last_rssi, bool *seen_uuid);
static void cleanup_stale_connections(void);                                                                   // Nueva función para limpiar conexiones obsoletas
static bool is_device_state_consistent(ble_device_info_t *device);                                             // Nueva función para verificar consistencia de estado
static void force_consistent_state(ble_device_info_t *device);                                                 // Nueva función para forzar estado consistente
static esp_err_t load_discovered_devices_safe(void);                                                           // Carga dispositivos conocidos desde NVS con protección
static esp_err_t load_known_profiles_safe(void);                                                               // Carga perfiles conocidos desde NVS con protección
static int on_mtu_exchange(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg); // Callback de MTU exchange
static bool is_service_in_known_profiles(const ble_uuid_t *uuid, int *out_profile_index);                      // Verifica si el servicio está en perfiles conocidos
static bool is_characteristic_in_known_profiles(const ble_uuid_t *uuid, int profile_index);                    // Verifica si la característica está en perfiles conocidos
static void mark_device_as_error_and_disconnect(ble_device_info_t *device);                                    // Marca dispositivo como error y desconecta
static bool ble_device_gap_discovery_active(void);
static void ble_device_drain_scan_complete_signal(void);
static esp_err_t ble_device_cancel_scan_and_wait(uint32_t timeout_ms);
static void schedule_stop_scan(void);                                                                          // Programa parada de escaneo desde la tarea worker
static void ble_control_worker_task(void *p);                                                                  // Tarea worker para operaciones BLE
static esp_err_t ble_control_worker_init(void);                                                                // Inicializa la tarea worker y su cola
static void ble_control_worker_deinit(void);                                                                   // Detiene la tarea worker y destruye su cola
static esp_err_t ble_device_stop_persistent_tasks(uint32_t timeout_ms);
static void ble_device_runtime_resources_delete(void);
static bool ble_smart_delay_or_stopped(uint32_t delay_ms);                                                     // Espera cancelable para la tarea inteligente
static void ble_device_control_disconnect_all_for_stop(void);                                                  // Termina conexiones activas durante stop
static void ble_device_control_reset_runtime_state(void);                                                      // Limpia estado local tras stop
static void force_cleanup_stuck_operations(void);                                                              // Fuerza limpieza de operaciones bloqueadas

static discovery_context_t *get_free_discovery_context(uint16_t conn_handle)
{
    for (int i = 0; i < BLE_DEVICE_MAX_DEVICES; i++)
    {
        if (!discovery_contexts[i].in_use)
        {
            discovery_contexts[i].conn_handle = conn_handle;
            discovery_contexts[i].start_time = xTaskGetTickCount();
            discovery_contexts[i].services_discovered = false;
            discovery_contexts[i].characteristics_discovered = false;
            discovery_contexts[i].in_use = true;
            active_discoveries++;
            return &discovery_contexts[i];
        }
    }
    return NULL;
}

/**
 * @brief Configuración de descubrimiento inteligente
 * Esta estructura define los parámetros de descubrimiento inteligente.
 * Permite ajustar intervalos, número de dispositivos objetivo y si se envían comandos de prueba.
 */
typedef struct
{
    uint32_t scan_interval_normal_ms;      // Intervalo normal entre escaneos
    uint32_t scan_interval_maintenance_ms; // Intervalo en modo mantenimiento
    int max_retries;                       // Máximo reintentos por operación
    int target_devices;                    // Número objetivo de dispositivos
    bool auto_test_commands;               // Habilitar comandos de prueba automáticos
    bool maintenance_mode;                 // Modo mantenimiento (menos agresivo)
} smart_discovery_config_t;

/**
 * @brief Configuración de descubrimiento inteligente
 * Esta estructura define los parámetros de descubrimiento inteligente.
 * Permite ajustar intervalos, número de dispositivos objetivo y si se envían comandos de prueba.
 */
static smart_discovery_config_t smart_config;

// Estadísticas y control
typedef struct
{
    uint32_t total_cycles;
    uint32_t successful_discoveries;
    uint32_t failed_discoveries;
    uint32_t devices_connected_total;
    uint32_t last_successful_connection_time;
} smart_discovery_stats_t;

static smart_discovery_stats_t smart_stats = {0};

static bool ble_smart_delay_or_stopped(uint32_t delay_ms)
{
    const TickType_t chunk = pdMS_TO_TICKS(100);
    TickType_t remaining = pdMS_TO_TICKS(delay_ms);

    while (remaining > 0)
    {
        if (!smart_discovery_enabled || module_stopping)
        {
            return false;
        }

        TickType_t step = MIN(remaining, chunk);
        vTaskDelay(step);
        remaining -= step;
    }

    return smart_discovery_enabled && !module_stopping;
}

static void schedule_stop_scan(void)
{
    if (module_stopping || !ble_control_queue)
        return;
    ble_control_op_t op = {0};
    op.type = BLE_OP_STOP_SCAN;
    if (xQueueSend(ble_control_queue, &op, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "schedule_stop_scan: cola llena, intentando 100ms");
        xQueueSend(ble_control_queue, &op, pdMS_TO_TICKS(100));
    }
}

static void ble_control_worker_task(void *p)
{
    (void)p;
    ble_control_op_t op;

    ESP_LOGD(TAG, "Worker BLE persistente iniciado.");

    while (true)
    {
        if (xQueueReceive(ble_control_queue, &op, portMAX_DELAY) == pdTRUE)
        {
            switch (op.type)
            {
            case BLE_OP_DISCONNECT:
            {
                ESP_LOGD(TAG, "Worker: procesando desconexión programada para %02X:%02X:%02X:%02X:%02X:%02X",
                         op.addr[5], op.addr[4], op.addr[3], op.addr[2], op.addr[1], op.addr[0]);
                // Llamada segura: esta task está en contexto de app
                ble_device_disconnect(op.addr);
            }
            break;

            case BLE_OP_MARK_ERROR_DISCONNECT:
            {
                ESP_LOGD(TAG, "Worker: procesando MARK_ERROR_DISCONNECT para %02X:%02X:%02X:%02X:%02X:%02X",
                         op.addr[5], op.addr[4], op.addr[3], op.addr[2], op.addr[1], op.addr[0]);
                // Primero actualizar estado (seguro en contexto de app)
                update_device_state(op.addr, BLE_DEVICE_STATE_ERROR);
                // Luego, forzar desconexión si hay handle
                ble_device_disconnect(op.addr);
            }
            break;

            case BLE_OP_STOP_SCAN:
            {
                ESP_LOGD(TAG, "Worker: procesando STOP_SCAN programado");
                ble_device_stop_scan();
            }
            break;

            case BLE_OP_SHUTDOWN:
                ESP_LOGI(TAG, "Worker BLE recibió señal de apagado");
                goto task_exit;

            default:
                ESP_LOGW(TAG, "Worker: op desconocida %d", op.type);
                break;
            }
        }
    }

task_exit:
    ble_control_worker_handle = NULL;
    if (ble_control_worker_stop_signal != NULL)
    {
        xSemaphoreGive(ble_control_worker_stop_signal);
    }
    vTaskDelete(NULL);
}

static esp_err_t ble_control_worker_init(void)
{
    ble_log_memory_snapshot("worker_init:entry");

    if (ble_control_queue == NULL)
    {
        ble_control_queue = xQueueCreate(BLE_CONTROL_QUEUE_LEN, BLE_CONTROL_QUEUE_ITEM_SIZE);
        if (!ble_control_queue)
        {
            ESP_LOGE(TAG, "No se pudo crear ble_control_queue");
            ble_log_memory_snapshot("worker_init:queue_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ble_log_memory_snapshot("worker_init:queue_ready");

    if (ble_control_worker_stop_signal == NULL)
    {
        ble_control_worker_stop_signal = xSemaphoreCreateBinary();
        if (ble_control_worker_stop_signal == NULL)
        {
            ESP_LOGE(TAG, "No se pudo crear ble_control_worker_stop_signal");
            ble_log_memory_snapshot("worker_init:stop_signal_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (ble_control_worker_handle == NULL)
    {
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            ble_control_worker_task,
            "ble_ctrl_worker",
            BLE_CONTROL_WORKER_STACK_BYTES,
            NULL,
            BLE_CONTROL_WORKER_PRIORITY,
            &ble_control_worker_handle,
            BLE_RUNTIME_TASK_CORE,
            BLE_RUNTIME_TASK_STACK_CAPS);

        if (rc != pdPASS)
        {
            ESP_LOGE(TAG, "Fallo creando ble_control_worker_task en PSRAM");
            ble_log_memory_snapshot("worker_init:task_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ble_log_memory_snapshot("worker_init:task_ready");
    return ESP_OK;
}

static void ble_control_worker_deinit(void)
{
    if (ble_control_queue == NULL)
    {
        return;
    }

    ble_control_op_t discarded;
    while (xQueueReceive(ble_control_queue, &discarded, 0) == pdTRUE)
    {
        /* Drain stale work. Worker task remains persistent and blocked. */
    }
}

static bool ble_device_gap_discovery_active(void)
{
    return ble_common_is_synced() && ble_gap_disc_active();
}

static void ble_device_drain_scan_complete_signal(void)
{
    if (scan_complete_semaphore == NULL)
    {
        return;
    }

    while (xSemaphoreTake(scan_complete_semaphore, 0) == pdTRUE)
    {
        /* Drain stale completion signals before starting or cancelling a scan. */
    }
}

static esp_err_t ble_device_cancel_scan_and_wait(uint32_t timeout_ms)
{
    const uint32_t wait_ms = (timeout_ms == 0) ? 1000 : timeout_ms;

    if (!scanning_active && !ble_device_gap_discovery_active())
    {
        return ESP_OK;
    }

    ble_device_drain_scan_complete_signal();

    int rc = ble_gap_disc_cancel();
    if (rc != 0)
    {
        if (!ble_device_gap_discovery_active())
        {
            scanning_active = false;
            ESP_LOGI(TAG, "GAP discovery already idle during scan cancel (rc=%d)", rc);
            if (scan_complete_semaphore != NULL)
            {
                xSemaphoreGive(scan_complete_semaphore);
            }
            return ESP_OK;
        }

        ESP_LOGE(TAG, "ble_gap_disc_cancel() failed while GAP is still active: %d", rc);
        return ESP_FAIL;
    }

    if (scan_complete_semaphore != NULL &&
        xSemaphoreTake(scan_complete_semaphore, pdMS_TO_TICKS(wait_ms)) == pdTRUE)
    {
        scanning_active = false;
        ESP_LOGI(TAG, "GAP discovery cancel acknowledged by DISC_COMPLETE");
        return ESP_OK;
    }

    if (!ble_device_gap_discovery_active())
    {
        scanning_active = false;
        ESP_LOGI(TAG, "GAP discovery cancel completed; stack reports scanner idle");
        if (scan_complete_semaphore != NULL)
        {
            xSemaphoreGive(scan_complete_semaphore);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Timed out waiting for GAP discovery cancellation acknowledgement");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ble_device_stop_persistent_tasks(uint32_t timeout_ms)
{
    esp_err_t result = ESP_OK;
    const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);

    smart_discovery_enabled = false;
    smart_task_shutdown_requested = true;

    if (scanning_active)
    {
        esp_err_t stop_err = ble_device_stop_scan();
        if (stop_err != ESP_OK)
        {
            result = stop_err;
        }
    }

    if (smart_ble_task_handle != NULL)
    {
        if (smart_task_idle_signal != NULL)
        {
            xSemaphoreTake(smart_task_idle_signal, 0);
        }
        if (smart_task_start_signal != NULL)
        {
            xSemaphoreGive(smart_task_start_signal);
        }
        if (smart_task_idle_signal != NULL &&
            xSemaphoreTake(smart_task_idle_signal, wait_ticks) != pdTRUE)
        {
            ESP_LOGE(TAG, "Timeout esperando salida de smart BLE task");
            result = ESP_ERR_TIMEOUT;
        }
    }

    if (ble_control_worker_handle != NULL && ble_control_queue != NULL)
    {
        if (ble_control_worker_stop_signal != NULL)
        {
            xSemaphoreTake(ble_control_worker_stop_signal, 0);
        }

        ble_control_op_t op = {0};
        op.type = BLE_OP_SHUTDOWN;
        if (xQueueSend(ble_control_queue, &op, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGE(TAG, "No se pudo enviar señal de apagado a ble_ctrl_worker");
            result = ESP_FAIL;
        }
        else if (ble_control_worker_stop_signal != NULL &&
                 xSemaphoreTake(ble_control_worker_stop_signal, wait_ticks) != pdTRUE)
        {
            ESP_LOGE(TAG, "Timeout esperando salida de ble_ctrl_worker");
            result = ESP_ERR_TIMEOUT;
        }
    }

    return result;
}

static void ble_device_runtime_resources_delete(void)
{
    if (ble_control_queue != NULL)
    {
        vQueueDelete(ble_control_queue);
        ble_control_queue = NULL;
    }
    if (ble_control_worker_stop_signal != NULL)
    {
        vSemaphoreDelete(ble_control_worker_stop_signal);
        ble_control_worker_stop_signal = NULL;
    }
    if (smart_task_start_signal != NULL)
    {
        vSemaphoreDelete(smart_task_start_signal);
        smart_task_start_signal = NULL;
    }
    if (smart_task_idle_signal != NULL)
    {
        vSemaphoreDelete(smart_task_idle_signal);
        smart_task_idle_signal = NULL;
    }
    if (scan_complete_semaphore != NULL)
    {
        vSemaphoreDelete(scan_complete_semaphore);
        scan_complete_semaphore = NULL;
    }
    if (identity_validation_mutex != NULL)
    {
        vSemaphoreDelete(identity_validation_mutex);
        identity_validation_mutex = NULL;
    }
    if (devices_mutex != NULL)
    {
        vSemaphoreDelete(devices_mutex);
        devices_mutex = NULL;
    }

    ble_runtime_resources_ready = false;
    smart_task_shutdown_requested = false;
}

static esp_err_t ble_device_runtime_resources_init(void)
{
    if (ble_runtime_resources_ready)
    {
        return ESP_OK;
    }

    smart_task_shutdown_requested = false;

    if (devices_mutex == NULL)
    {
        devices_mutex = xSemaphoreCreateMutex();
        if (devices_mutex == NULL)
        {
            ESP_LOGE(TAG, "Error creando el mutex de dispositivos.");
            ble_log_memory_snapshot("runtime:devices_mutex_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (scan_complete_semaphore == NULL)
    {
        scan_complete_semaphore = xSemaphoreCreateBinary();
        if (scan_complete_semaphore == NULL)
        {
            ESP_LOGE(TAG, "Error creando scan_complete_semaphore.");
            ble_log_memory_snapshot("runtime:scan_sem_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (identity_validation_mutex == NULL)
    {
        identity_validation_mutex = xSemaphoreCreateMutex();
        if (identity_validation_mutex == NULL)
        {
            ESP_LOGE(TAG, "Error creando identity_validation_mutex.");
            ble_log_memory_snapshot("runtime:identity_mutex_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = ble_control_worker_init();
    if (err != ESP_OK)
    {
        return err;
    }

    if (smart_task_start_signal == NULL)
    {
        smart_task_start_signal = xSemaphoreCreateBinary();
        if (smart_task_start_signal == NULL)
        {
            ESP_LOGE(TAG, "Error creando smart_task_start_signal.");
            ble_log_memory_snapshot("runtime:smart_start_sem_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (smart_task_idle_signal == NULL)
    {
        smart_task_idle_signal = xSemaphoreCreateBinary();
        if (smart_task_idle_signal == NULL)
        {
            ESP_LOGE(TAG, "Error creando smart_task_idle_signal.");
            ble_log_memory_snapshot("runtime:smart_idle_sem_failed");
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(smart_task_idle_signal);
    }

    if (smart_ble_task_handle == NULL)
    {
        ble_log_memory_snapshot("runtime:smart_task_before_create");

        BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
            smart_ble_discovery_btdevices_task,
            "smart_ble",
            BLE_SMART_TASK_STACK_BYTES,
            NULL,
            BLE_SMART_TASK_PRIORITY,
            &smart_ble_task_handle,
            BLE_RUNTIME_TASK_CORE,
            BLE_RUNTIME_TASK_STACK_CAPS);

        if (task_ret != pdPASS)
        {
            ESP_LOGE(TAG, "Error creando tarea BLE inteligente persistente: %d", task_ret);
            smart_ble_task_handle = NULL;
            ble_log_memory_snapshot("runtime:smart_task_failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ble_runtime_resources_ready = true;
    ble_log_memory_snapshot("runtime:ready");
    return ESP_OK;
}

static void ble_device_control_disconnect_all_for_stop(void)
{
    uint16_t handles[BLE_DEVICE_MAX_DEVICES] = {0};
    int handle_count = 0;

    if (devices_mutex != NULL &&
        xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        for (int i = 0; i < discovered_count && handle_count < BLE_DEVICE_MAX_DEVICES; i++)
        {
            uint16_t conn_handle = discovered_devices[i].conn_handle;
            if (conn_handle != BLE_HS_CONN_HANDLE_NONE && conn_handle != 0)
            {
                handles[handle_count++] = conn_handle;
                discovered_devices[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
                discovered_devices[i].state = BLE_DEVICE_STATE_DISCONNECTED;
                discovered_devices[i].char_val_handle = 0;
                discovered_devices[i].pairing_char_handle = 0;
                discovered_devices[i].pairing_char_found = false;
                discovered_devices[i].control_char_found = false;
                discovered_devices[i].char_discovered = false;
            }
        }
        xSemaphoreGive(devices_mutex);
    }

    for (int i = 0; i < handle_count; i++)
    {
        int rc = ble_gap_terminate(handles[i], BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN && rc != BLE_HS_EINVAL)
        {
            ESP_LOGW(TAG, "ble_gap_terminate(handle=%u) durante stop devolvio %d", (unsigned)handles[i], rc);
        }
    }

    for (int wait_ms = 0; wait_ms < 1000 && handle_count > 0; wait_ms += 50)
    {
        bool any_connected = false;
        for (int i = 0; i < handle_count; i++)
        {
            struct ble_gap_conn_desc conn_desc;
            if (ble_gap_conn_find(handles[i], &conn_desc) == 0)
            {
                any_connected = true;
                break;
            }
        }

        if (!any_connected)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ble_device_control_reset_runtime_state(void)
{
    memset(discovered_devices, 0, sizeof(discovered_devices));
    discovered_count = 0;
    memset(discovery_contexts, 0, sizeof(discovery_contexts));
    active_discoveries = 0;
    memset(&best_candidate, 0, sizeof(best_candidate));
    memset(&g_pending_connection, 0, sizeof(g_pending_connection));
    atomic_store(&g_active_ble_operations, 0);
    nvs_loading_complete = false;
    scanning_active = false;
    smart_discovery_enabled = false;
    smart_discovery_running = false;
}

static bool ble_device_full_release_already_off(void)
{
    const bool runtime_resources_released =
        !ble_runtime_resources_ready &&
        smart_ble_task_handle == NULL &&
        smart_task_start_signal == NULL &&
        smart_task_idle_signal == NULL &&
        scan_complete_semaphore == NULL &&
        identity_validation_mutex == NULL &&
        devices_mutex == NULL &&
        ble_control_queue == NULL &&
        ble_control_worker_handle == NULL &&
        ble_control_worker_stop_signal == NULL;

    const bool device_runtime_idle =
        !module_initialized &&
        !smart_discovery_enabled &&
        !smart_discovery_running &&
        !scanning_active;

    const bool common_released =
        !ble_common_is_started() &&
        ble_common_get_owner() == BLE_COMMON_ROLE_NONE &&
        ble_common_get_state() == BLE_COMMON_STATE_UNINITIALIZED;

    return runtime_resources_released &&
           device_runtime_idle &&
           common_released;
}

static void schedule_mark_error_and_disconnect(const uint8_t addr[6])
{
    if (module_stopping || !ble_control_queue)
        return;
    ble_control_op_t op = {0};
    op.type = BLE_OP_MARK_ERROR_DISCONNECT;
    memcpy(op.addr, addr, 6);
    if (xQueueSend(ble_control_queue, &op, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "schedule_mark_error_and_disconnect: cola llena, intentando 100ms");
        xQueueSend(ble_control_queue, &op, pdMS_TO_TICKS(100));
    }
}

static void mark_device_as_error_and_disconnect(ble_device_info_t *device)
{
    if (!device)
        return;
    schedule_mark_error_and_disconnect(device->addr);
    ESP_LOGW(TAG, "Queued mark-error+disconnect for %s", device->name ? device->name : "(no name)");
}

static esp_err_t load_discovered_devices_safe(void)
{
    esp_err_t result = ESP_OK;
    bool mutex_taken = false;
    device_profile_nvs_t *loaded_profiles = NULL;

    const char *current_ssid = wifi_session_get_connected_ssid();
    if (current_ssid == NULL || *current_ssid == '\0')
    {
        ESP_LOGI(TAG, "[BLE_NVS] No hay SSID activo, usando lista vacía.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Cargando perfiles de dispositivos para SSID: %s", current_ssid);

    // Verificar mutex antes de proceder
    if (devices_mutex == NULL)
    {
        ESP_LOGE(TAG, "[BLE_NVS] devices_mutex no existe - abortando carga");
        return ESP_ERR_INVALID_STATE;
    }

    // Array temporal para no bloquear la lista global durante la carga NVS
    // --- CAMBIO CLAVE: Asignar memoria del HEAP, no de la PILA ---
    loaded_profiles = heap_caps_malloc(sizeof(device_profile_nvs_t) * BLE_DEVICE_MAX_DEVICES, BLE_STAGING_BUFFER_CAPS);
    if (loaded_profiles == NULL)
    {
        ESP_LOGE(TAG, "[BLE_NVS] No se pudo asignar memoria PSRAM para cargar perfiles desde NVS");
        return ESP_ERR_NO_MEM;
    }
    // ----------------------------------------------------------------
    const int loaded_count = load_devices_for_ssid(current_ssid, loaded_profiles, BLE_DEVICE_MAX_DEVICES);

    if (loaded_count <= 0)
    {
        ESP_LOGI(TAG, "No hay dispositivos guardados para este SSID.");
        goto cleanup;
    }

    // Tomar mutex SÓLO cuando ya tenemos los datos y vamos a modificar la lista global
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "[BLE_NVS] Timeout tomando mutex para escribir en la lista - abortando carga en memoria");
        result = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    mutex_taken = true;

    // Limpiar array antes de cargar
    memset(discovered_devices, 0, sizeof(discovered_devices));
    discovered_count = 0;

    // Cargar dispositivos uno por uno con validación
    for (int i = 0; i < loaded_count && i < BLE_DEVICE_MAX_DEVICES; i++)
    {
        ble_device_info_t *dev = &discovered_devices[discovered_count];

        if (loaded_profiles[i].name[0] == '\0')
        {
            ESP_LOGW(TAG, "[BLE_NVS] Saltando dispositivo %d: nombre vacío", i);
            continue;
        }

        memcpy(dev->addr, loaded_profiles[i].addr.val, sizeof(dev->addr));
        dev->addr_type = loaded_profiles[i].addr.type;
        strlcpy(dev->name, loaded_profiles[i].name, sizeof(dev->name));
        dev->type = (ble_device_type_t)loaded_profiles[i].device_type;
        dev->service_uuid_128 = loaded_profiles[i].service_uuid;
        dev->char_uuid_128 = loaded_profiles[i].char_uuid;

        // Inicializar campos de estado
        dev->rssi = 0;
        dev->last_seen = 0;
        dev->state = BLE_DEVICE_STATE_DISCONNECTED;
        dev->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        dev->is_known = true; // Si se carga de NVS, es conocido

        // Limpieza de otros campos
        dev->char_val_handle = 0;
        dev->pairing_char_handle = 0;
        dev->char_discovered = false;
        dev->matched_profile_index = -1;

        ESP_LOGD(TAG, "Dispositivo restaurado: %s %02X:%02X:%02X:%02X:%02X:%02X",
                 dev->name, dev->addr[5], dev->addr[4], dev->addr[3],
                 dev->addr[2], dev->addr[1], dev->addr[0]);

        discovered_count++;
    }

    ESP_LOGI(TAG, "Cargados %d dispositivos para SSID: %s", discovered_count, current_ssid);

cleanup:
    if (mutex_taken)
    {
        xSemaphoreGive(devices_mutex);
    }
    if (loaded_profiles != NULL)
    {
        heap_caps_free(loaded_profiles);
    }
    return result;
}

static esp_err_t load_known_profiles_safe(void)
{
    nvs_handle_t nvs_handle = 0;
    bool nvs_opened = false;
    esp_err_t ret = ESP_OK;

    // --- CAMBIO CLAVE 2: USAR EL MUTEX PERSONALIZADO ---
    nvs_lock(); // Bloquea el acceso a NVS

    ret = nvs_open("ble_profiles", NVS_READONLY, &nvs_handle);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "No se encontró base de datos de perfiles en NVS. Usando vacía.");
        g_known_profiles_count = 0;
        goto cleanup;
    }

    nvs_opened = true;

    size_t required_size = 0;
    ret = nvs_get_blob(nvs_handle, "profiles_db", NULL, &required_size);

    if (ret == ESP_OK && required_size > 0 && required_size <= sizeof(g_known_profiles))
    {
        ret = nvs_get_blob(nvs_handle, "profiles_db", g_known_profiles, &required_size);
        if (ret == ESP_OK)
        {
            g_known_profiles_count = required_size / sizeof(known_device_profile_t);
            ESP_LOGI(TAG, "✅ Cargados %d perfiles de dispositivos conocidos desde NVS.",
                     g_known_profiles_count);
        }
        else
        {
            ESP_LOGE(TAG, "Error leyendo blob de perfiles: %s", esp_err_to_name(ret));
            g_known_profiles_count = 0; // Usar lista vacía en caso de error
        }
    }
    else if (ret == ESP_ERR_NVS_NOT_FOUND || required_size == 0)
    {
        g_known_profiles_count = 0;
    }
    else
    {
        ESP_LOGE(TAG, "Error obteniendo tamaño de blob de perfiles: %s", esp_err_to_name(ret));
        g_known_profiles_count = 0;
    }

cleanup:
    if (nvs_opened)
    {
        nvs_close(nvs_handle);
    }
    nvs_unlock();
    return ESP_OK; // Siempre devolver OK para no bloquear el sistema
}

static int count_known_onboarded_devices(void)
{
    int count = 0;
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 0; i < discovered_count; i++)
        {
            if (discovered_devices[i].is_known)
            {
                count++;
            }
        }
        xSemaphoreGive(devices_mutex);
    }
    return count;
}

static bool target_devices_reached(void)
{
    // El objetivo se alcanza si el número de dispositivos conocidos es igual o mayor al configurado.
    return (count_known_onboarded_devices() >= smart_config.target_devices);
}

/**
 * @brief Intenta reconectar dispositivos desconectados inesperadamente
 */
static void attempt_device_reconnection(void)
{
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return;
    }

    for (int i = 0; i < discovered_count; i++)
    {
        ble_device_info_t *device = &discovered_devices[i];

        // Solo intentar reconectar si el dispositivo es conocido y está desconectado
        if (device->is_known && device->state == BLE_DEVICE_STATE_DISCONNECTED)
        {

            // --- REEMPLAZA EL BLOQUE ANTIGUO CON ESTO ---
            ESP_LOGI(TAG, "Intentando reconectar con dispositivo conocido: %s", device->name);
            ble_device_connect(device->addr, device->addr_type);
            // Salimos del bucle para manejar una conexión a la vez
            break;
            // ---------------------------------------------
        }
    }

    xSemaphoreGive(devices_mutex);
}

/**
 * @brief Limpia conexiones obsoletas
 * Esta función revisa los dispositivos con connection handle asignado
 * y verifica si la conexión aún existe. Si no, limpia el handle y actualiza el estado.
 */
static void cleanup_stale_connections(void)
{
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        for (int i = 0; i < discovered_count; i++)
        {
            if (discovered_devices[i].conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                discovered_devices[i].conn_handle != 0 &&
                (discovered_devices[i].state == BLE_DEVICE_STATE_CONNECTED ||
                 discovered_devices[i].state == BLE_DEVICE_STATE_CONNECTING))
            {

                struct ble_gap_conn_desc conn_desc;
                if (ble_gap_conn_find(discovered_devices[i].conn_handle, &conn_desc) != 0)
                {
                    ESP_LOGW(TAG, "Limpiando handle obsoleto para %s", discovered_devices[i].name);
                    discovered_devices[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
                    discovered_devices[i].state = BLE_DEVICE_STATE_DISCONNECTED;
                }
            }
        }
        xSemaphoreGive(devices_mutex);
    }
}

/**
 * @brief Initialize BLE device control module
 * @param callbacks Callbacks for device events
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ble_device_control_start(ble_device_callbacks_t *callbacks)
{
    ble_log_memory_snapshot("start:entry");

    if (module_initialized)
    {
        ESP_LOGI(TAG, "Módulo de Control de Dispositivos ya inicializado.");
        if (ble_common_get_owner() == BLE_COMMON_ROLE_NONE)
        {
            esp_err_t err = ble_common_acquire(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "No se pudo readquirir ownership BLE Central: %s", esp_err_to_name(err));
                return err;
            }
        }
        else if (ble_common_get_owner() != BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC)
        {
            ESP_LOGE(TAG, "BLE Central no puede iniciar; owner actual=%d", ble_common_get_owner());
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    if (!ble_central_diagnostic_enabled())
    {
        ESP_LOGW(TAG, "BLE Central diagnostic is disabled by default");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ble_require_internal_memory_floor("start:entry");
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "=== Iniciando Módulo de Control de Dispositivos (Modo Escáner) ===");

    if (!ble_common_is_synced())
    {
        ESP_LOGE(TAG, "BLE Central start rejected; NimBLE host is not synced");
        return ESP_ERR_INVALID_STATE;
    }

    err = ble_common_acquire(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE Central ownership rejected: %s", esp_err_to_name(err));
        return err;
    }

    ble_log_memory_snapshot("start:owner_acquired");

    module_stopping = false;
    smart_discovery_enabled = false;
    smart_discovery_running = false;
    scanning_active = false;
    nvs_loading_complete = false;

    nvs_setup_mutex_init();

    if (callbacks)
    {
        memcpy(&device_callbacks, callbacks, sizeof(ble_device_callbacks_t));
    }

    err = ble_device_runtime_resources_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize persistent BLE runtime resources: %s", esp_err_to_name(err));
        ble_common_release(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);
        module_stopping = false;
        return err;
    }

    ble_log_memory_snapshot("start:runtime_ready");

    // Inicializar listas
    memset(discovered_devices, 0, sizeof(discovered_devices));
    discovered_count = 0;

    // --- SECUENCIA DE CARGA SÍNCRONA Y SEGURA ---
    // Estas funciones ahora se ejecutan aquí, bloqueando el flujo hasta que terminan.
    // Esto ocurre ANTES de que la tarea `smart_ble_discovery_btdevices_task` sea creada.
    ESP_LOGI(TAG, "[BLE_NVS] Iniciando carga síncrona de datos BLE...");

    // Carga #1: Perfiles
    load_known_profiles_safe();
    ble_log_memory_snapshot("start:known_profiles_loaded");

    // Carga #2: Dispositivos guardados para el SSID actual
    // (Usando la lógica simplificada que ya habíamos implementado)
    const char *current_ssid = wifi_session_get_connected_ssid();
    if (current_ssid != NULL && *current_ssid != '\0')
    {
        esp_err_t load_result = load_discovered_devices_safe();
        if (load_result != ESP_OK)
        {
            ESP_LOGE(TAG, "[BLE_NVS] Falló la carga de dispositivos guardados con error: %s", esp_err_to_name(load_result));
            // Decisión de diseño: ¿Continuamos con una lista vacía o retornamos un error?
            // Por ahora, continuamos para que el resto del sistema pueda arrancar,
            // pero el error queda registrado.
        }
    }
    else
    {
        ESP_LOGW(TAG, "[BLE_NVS] No hay SSID conectado, no se cargarán dispositivos guardados.");
    }

    ble_log_memory_snapshot("start:discovered_devices_loaded");

    ESP_LOGI(TAG, "[BLE_NVS] Carga síncrona completada.");
    nvs_loading_complete = true;

    module_initialized = true;
    ble_log_memory_snapshot("start:complete");
    ESP_LOGI(TAG, "Módulo BLE Device Control inicializado correctamente (inicio no bloqueante).");
    return ESP_OK;
}

/**
 * @brief Deinitialize BLE device control module
 */
// Esta función detiene el módulo y libera todos los recursos
void ble_device_control_stop(void)
{
    if (!module_initialized && !smart_discovery_running && !scanning_active)
    {
        return;
    }

    ESP_LOGI(TAG, "Deteniendo Módulo de Control de Dispositivos...");

    module_stopping = true;
    smart_discovery_enabled = false;

    if (scanning_active)
    {
        esp_err_t stop_err = ble_device_stop_scan();
        if (stop_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Scan stop during control stop failed: %s", esp_err_to_name(stop_err));
        }
    }

    ble_device_stop_smart_task();

    ble_control_worker_deinit();

    ble_device_control_disconnect_all_for_stop();

    if (devices_mutex != NULL &&
        xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        ble_device_control_reset_runtime_state();
        xSemaphoreGive(devices_mutex);
    }
    else
    {
        ble_device_control_reset_runtime_state();
    }

    memset(&device_callbacks, 0, sizeof(ble_device_callbacks_t));

    module_initialized = false;
    nvs_loading_complete = false;
    ble_common_release(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);

    module_stopping = false;

    ESP_LOGI(TAG, "BLE Device Control desactivado; recursos persistentes conservados");
}

esp_err_t ble_device_full_release(uint32_t timeout_ms)
{
    const uint32_t wait_ms = (timeout_ms == 0) ? 5000 : timeout_ms;

    if (ble_device_full_release_already_off())
    {
        module_stopping = false;
        smart_task_shutdown_requested = false;
        ESP_LOGD(TAG, "BLE full release ignored; stack and runtime already stopped");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Iniciando liberacion completa de BLE para handover de audio (%lu ms)", wait_ms);
    ble_log_memory_snapshot("full_release:entry");

    ble_wifi_provisioning_deinit();

    if (module_initialized || smart_discovery_running || scanning_active)
    {
        ble_device_control_stop();
    }

    module_stopping = true;

    esp_err_t err = ble_device_stop_persistent_tasks(wait_ms);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudieron detener tareas BLE persistentes: %s", esp_err_to_name(err));
        module_stopping = false;
        ble_log_memory_snapshot("full_release:tasks_failed");
        return err;
    }

    ble_device_control_disconnect_all_for_stop();

    err = ble_common_deinit(wait_ms);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo desinicializar NimBLE: %s", esp_err_to_name(err));
        module_stopping = false;
        ble_log_memory_snapshot("full_release:nimble_failed");
        return err;
    }

    ble_device_control_reset_runtime_state();
    memset(&device_callbacks, 0, sizeof(ble_device_callbacks_t));
    module_initialized = false;
    nvs_loading_complete = false;
    module_stopping = false;

    ble_device_runtime_resources_delete();

    ble_log_memory_snapshot("full_release:complete");
    ESP_LOGI(TAG, "Liberacion completa de BLE finalizada");
    return ESP_OK;
}

/**
 * @brief Start scanning for BLE devices
 */
esp_err_t ble_device_start_scan(uint32_t timeout_ms)
{
    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Módulo no está inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    if (scanning_active)
    {
        ESP_LOGW(TAG, "Escaneo ya está activo");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Iniciando escaneo de dispositivos BLE...");

    // Configurar parámetros de escaneo
    struct ble_gap_disc_params disc_params = {
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .itvl = 0x10,   // 10ms
        .window = 0x10, // 10ms
        .filter_duplicates = 1,
    };

    // Iniciar escaneo (BLE_HS_FOREVER o timeout_ms en ms)
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          timeout_ms == 0 ? BLE_HS_FOREVER : timeout_ms,
                          &disc_params,
                          ble_gap_scan_event_handler,
                          NULL);

    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error iniciando escaneo: %d", rc);
        return ESP_FAIL;
    }

    scanning_active = true;
    ESP_LOGI(TAG, "Escaneo iniciado (timeout: %lu ms)", timeout_ms);

    return ESP_OK;
}

/**
 * @brief Stop scanning for BLE devices
 */
esp_err_t ble_device_stop_scan(void)
{
    // Si no hay escaneo activo, devolvemos OK (idempotente)
    if (!scanning_active && !ble_device_gap_discovery_active())
    {
        ESP_LOGD(TAG, "ble_device_stop_scan(): no había escaneo activo");
        return ESP_OK;
    }

    esp_err_t err = ble_device_cancel_scan_and_wait(1000);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Escaneo no pudo detenerse limpiamente: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Escaneo detenido; GAP discovery idle.");
    return ESP_OK;
}

/**
 * @brief Connect to a BLE device
 */
esp_err_t ble_device_connect(uint8_t device_addr[6], uint8_t addr_type)
{
    if (!module_initialized || module_stopping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Buscar dispositivo en la lista
    ble_device_info_t *device = find_device_by_addr_internal(device_addr);
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo no encontrado en la lista");
        return ESP_ERR_NOT_FOUND;
    }

    // NUEVA VALIDACIÓN: Verificar si ya existe una conexión activa
    if (device->conn_handle != BLE_HS_CONN_HANDLE_NONE && device->conn_handle != 0)
    {
        // Verificar si la conexión realmente existe
        struct ble_gap_conn_desc conn_desc;
        if (ble_gap_conn_find(device->conn_handle, &conn_desc) == 0)
        {
            ESP_LOGW(TAG, "Dispositivo %s ya tiene conexión activa (handle: %d)",
                     device->name, device->conn_handle);
            return ESP_ERR_INVALID_STATE;
        }
        else
        {
            // La conexión no existe, limpiar el handle
            ESP_LOGW(TAG, "Limpiando handle obsoleto para %s", device->name);
            device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
    }
    // Validar estado actual
    if (device->state == BLE_DEVICE_STATE_CONNECTED)
    {
        ESP_LOGW(TAG, "Dispositivo ya está conectado");
        return ESP_OK;
    }

    if (device->state == BLE_DEVICE_STATE_CONNECTING)
    {
        ESP_LOGW(TAG, "Dispositivo ya está en proceso de conexión");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Conectando a dispositivo: %s", device->name);
    update_device_state(device_addr, BLE_DEVICE_STATE_CONNECTING);

    // Resto del código de conexión...
    struct ble_gap_conn_params conn_params = {0};
    conn_params.scan_itvl = 0x0010;
    conn_params.scan_window = 0x0010;
    conn_params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    conn_params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    conn_params.latency = 0;
    conn_params.supervision_timeout = 0x0100;
    conn_params.min_ce_len = 0x0010;
    conn_params.max_ce_len = 0x0300;

    ble_addr_t peer_addr;
    memcpy(peer_addr.val, device_addr, 6);
    peer_addr.type = addr_type;

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                             &peer_addr, // Esto sigue igual
                             BLE_DEVICE_CONNECT_TIMEOUT_MS,
                             &conn_params,
                             ble_gap_connect_event_handler,
                             NULL);

    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error iniciando conexión: %d", rc);
        update_device_state(device_addr, BLE_DEVICE_STATE_ERROR);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief BLE GAP scan event handler con auto-conexión integrada
 * Este manejador procesa eventos de escaneo BLE y maneja la auto-conexión inteligente.
 * Detecta dispositivos, actualiza su estado y maneja la auto-conexión si está habilitada.
 * @param event Evento de escaneo BLE
 * @param arg Argumento adicional (no se usa aquí)
 * @return 0 en caso de éxito, otro código en caso de error
 * @note Este manejador es llamado por la pila NimBLE cuando se detectan eventos de escaneo.
 *       Se encarga de procesar los dispositivos descubiertos, actualizar su estado y
 *       manejar la auto-conexión inteligente si está habilitada.
 */
static int ble_gap_scan_event_handler(struct ble_gap_event *event, void *arg)
{
    if (module_stopping)
    {
        if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
        {
            scanning_active = false;
            if (scan_complete_semaphore != NULL)
            {
                xSemaphoreGive(scan_complete_semaphore);
            }
        }
        return 0;
    }

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
    {
        ble_device_info_t device = {0};

        // Copiar dirección y tipo tal cual los entrega NimBLE
        memcpy(device.addr, event->disc.addr.val, sizeof(device.addr));
        device.addr_type = event->disc.addr.type;
        device.rssi = event->disc.rssi;
        device.last_seen = xTaskGetTickCount();
        device.state = BLE_DEVICE_STATE_DISCONNECTED;

        // Extraer nombre del dispositivo si está disponible
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0 && fields.name)
        {
            size_t name_len = MIN(fields.name_len, BLE_DEVICE_MAX_NAME_LEN - 1);
            memcpy(device.name, fields.name, name_len);
            device.name[name_len] = '\0';
        }
        else
        {
            snprintf(device.name, sizeof(device.name), "Unknown_%02X%02X",
                     device.addr[4], device.addr[5]);
        }

        // Detectar tipo
        device.type = ble_device_detect_type_from_name(device.name);

        ESP_LOGI(TAG, "🔍 Dispositivo encontrado: %s (RSSI: %d)", device.name, device.rssi);

        // Guardar en lista
        add_or_update_discovered_device(&device);

        // === AUTO-CONEXIÓN INTELIGENTE ===
        if (auto_connection_globally_enabled)
        {
            ble_device_info_t *device_in_list = find_device_by_addr_internal(device.addr);
            if (device_in_list)
            {
                if (device_in_list->processed_in_current_cycle)
                {
                    break; // Ya lo vimos en este ciclo, no hacer nada más
                }

                device_in_list->processed_in_current_cycle = true;

                // === Callback de notificación ===
                if (device_callbacks.on_device_discovered)
                {
                    device_callbacks.on_device_discovered(device_in_list);
                }

                // ¿Es un dispositivo nuevo y tiene mejor señal que nuestro candidato actual?
                if (!device_in_list->is_known &&
                    device_in_list->state != BLE_DEVICE_STATE_ERROR &&
                    device_in_list->rssi > best_candidate.rssi)
                {
                    ESP_LOGI(TAG, "⭐ Nuevo mejor candidato encontrado: %s (RSSI: %d)", device_in_list->name, device_in_list->rssi);
                    memcpy(best_candidate.addr, device_in_list->addr, 6);
                    best_candidate.addr_type = device_in_list->addr_type;
                    best_candidate.rssi = device_in_list->rssi;
                    best_candidate.found = true;
                }
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Escaneo completado");
        scanning_active = false;
        // Avisar a la tarea principal que el escaneo ha terminado.
        if (scan_complete_semaphore != NULL)
        {
            xSemaphoreGive(scan_complete_semaphore);
        }
        break;

    default:
        break;
    }

    return 0;
}

static int on_mtu_exchange(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    ble_device_info_t *device = (ble_device_info_t *)arg;

    if (!device)
    {
        ESP_LOGE(TAG, "Device pointer is NULL in MTU exchange callback");
        return -1;
    }

    if (error->status == 0)
    {
        ESP_LOGI(TAG, "MTU negociado a %d para '%s'", mtu, device->name);
        update_device_state(device->addr, BLE_DEVICE_STATE_DISCOVERING_SVCS);
    }
    else
    {
        ESP_LOGE(TAG, "Error en MTU exchange para '%s': %d", device->name, error->status);
        // NUEVA LÓGICA: No continuar si el error es crítico
        if (error->status == BLE_HS_ENOTCONN || error->status == BLE_HS_ENOTSYNCED)
        {
            ESP_LOGE(TAG, "Error crítico en MTU, terminando conexión para '%s'", device->name);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            mark_device_as_error_and_disconnect(device);
            return -1;
        }

        // Para otros errores, continuar con un MTU por defecto
        ESP_LOGW(TAG, "Continuando con MTU por defecto para '%s'", device->name);
    }

    // Proceder al descubrimiento de servicios directamente
    ESP_LOGI(TAG, "Iniciando descubrimiento de servicios para '%s'...", device->name);

    // Crear contexto de discovery
    discovery_context_t *ctx = get_free_discovery_context(conn_handle);
    if (!ctx)
    {
        ESP_LOGE(TAG, "No hay contextos de discovery disponibles");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        mark_device_as_error_and_disconnect(device);
        return -1;
    }

    int rc = ble_gattc_disc_all_svcs(conn_handle, on_service_discovered, device);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Fallo al iniciar descubrimiento de servicios: %d", rc);
        cleanup_discovery_context(conn_handle);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        mark_device_as_error_and_disconnect(device);
        return rc;
    }

    return 0;
}

static bool is_device_state_consistent(ble_device_info_t *device)
{
    if (!device)
        return false;

    switch (device->state)
    {
    case BLE_DEVICE_STATE_DISCONNECTED:
        return (device->conn_handle == BLE_HS_CONN_HANDLE_NONE ||
                device->conn_handle == 0) &&
               device->char_val_handle == 0 &&
               device->pairing_char_handle == 0;

    case BLE_DEVICE_STATE_CONNECTING:
        // Puede o no tener conn_handle durante conexión, pero aún no hay handles
        return device->char_val_handle == 0 &&
               device->pairing_char_handle == 0;

    case BLE_DEVICE_STATE_CONNECTED:
        return device->conn_handle != BLE_HS_CONN_HANDLE_NONE &&
               device->conn_handle != 0 &&
               device->pairing_char_handle == 0 &&
               device->char_val_handle == 0;

    case BLE_DEVICE_STATE_ERROR:
        return true; // Error state es válido en cualquier configuración

    default:
        return false;
    }
}

static void force_consistent_state(ble_device_info_t *device)
{
    if (!device)
        return;

    if (is_device_state_consistent(device))
        return;

    static uint32_t last_force_time = 0;
    uint32_t current_time = xTaskGetTickCount();

    if (current_time - last_force_time < pdMS_TO_TICKS(1000))
    {
        return;
    }
    last_force_time = current_time;

    ESP_LOGW(TAG, "Forzando estado consistente para device '%s'", device->name);

    if (device->conn_handle != BLE_HS_CONN_HANDLE_NONE && device->conn_handle != 0)
    {
        struct ble_gap_conn_desc conn_desc;
        if (ble_gap_conn_find(device->conn_handle, &conn_desc) != 0)
        {
            // Conexión no existe, limpiar estado
            device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            device->char_val_handle = 0;
            device->pairing_char_handle = 0;    // NEW
            device->pairing_char_found = false; // NEW
            device->control_char_found = false; // NEW
            device->char_discovered = false;    // NEW
            device->state = BLE_DEVICE_STATE_DISCONNECTED;
            ESP_LOGW(TAG, "Device '%s' forzado a DISCONNECTED", device->name);
        }
    }
    else
    {
        // Sin conexión válida
        device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        device->char_val_handle = 0;
        device->pairing_char_handle = 0;    // NEW
        device->pairing_char_found = false; // NEW
        device->control_char_found = false; // NEW
        device->char_discovered = false;    // NEW
        device->state = BLE_DEVICE_STATE_DISCONNECTED;
    }
}

/**
 * @brief BLE GAP connection event handler con manejo de errores mejorado
 * Este manejador procesa eventos de conexión BLE, maneja errores y asegura estados consistentes.
 * @param event Evento de conexión BLE
 * @param arg Argumento adicional (no se usa aquí)
 * @return 0 en caso de éxito, otro código en caso de error
 * @note Este manejador es llamado por la pila NimBLE cuando se detectan eventos de conexión.
 *       Se encarga de procesar conexiones, cambios de encriptación y desconexiones,
 *       asegurando que el estado del dispositivo sea consistente y manejando errores adecuadamente.
 */
static int ble_gap_connect_event_handler(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc conn_desc;
    ble_device_info_t *device = NULL;

    if (module_stopping)
    {
        if (event->type == BLE_GAP_EVENT_CONNECT ||
            event->type == BLE_GAP_EVENT_DISCONNECT)
        {
            g_pending_connection.is_active = false;
            atomic_store(&g_active_ble_operations, 0);
        }
        return 0;
    }

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Evento de conexión recibido (status: %d, handle: %d)",
                 event->connect.status, event->connect.conn_handle);

        // Limpiar el expediente sin importar el resultado, ya que la operación de "conectar" ha terminado
        g_pending_connection.is_active = false;

        if (event->connect.status == 0) // Éxito en la conexión
        {
            if (ble_gap_conn_find(event->connect.conn_handle, &conn_desc) == 0)
            {
                ESP_LOGI(TAG, "Conexion establecida (Encrypted: %d), Authenticated: %d, Bonded: %d",
                         conn_desc.sec_state.encrypted,
                         conn_desc.sec_state.authenticated,
                         conn_desc.sec_state.bonded);
                device = find_device_by_addr_internal(conn_desc.peer_ota_addr.val);
                if (device)
                {
                    device->conn_handle = event->connect.conn_handle;
                    device->char_val_handle = 0;
                    update_device_state(device->addr, BLE_DEVICE_STATE_CONNECTED);

                    if (device_callbacks.on_device_connected)
                    {
                        device_callbacks.on_device_connected(device);
                    }

                    // En lugar de forzar bonding inmediatamente, permite que ocurra naturalmente
                    ESP_LOGI(TAG, "Connection established for '%s' (handle: %d)", device->name, event->connect.conn_handle);

                    // NO forzar bonding - dejar que el foco lo inicie si es necesario
                    ESP_LOGI(TAG, "Saltando iniciación manual de bonding. Procediendo con MTU...");
                    int rc_mtu = ble_gattc_exchange_mtu(device->conn_handle, on_mtu_exchange, device);
                    if (rc_mtu != 0)
                    {
                        ESP_LOGE(TAG, "Error iniciando MTU exchange: %d", rc_mtu);
                        ble_gap_terminate(device->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    }
                }
            }
        }
        else // Error en la conexión
        {
            // MANEJO MEJORADO DE ERRORES DE CONEXIÓN
            ESP_LOGE(TAG, "Error en conexión: %d", event->connect.status);

            // Buscar dispositivo que estaba conectando
            // Usamos la dirección guardada en nuestro expediente.
            ble_device_info_t *failed_device = find_device_by_addr_internal(g_pending_connection.addr);
            if (failed_device)
            {
                // Si el dispositivo no era conocido, estábamos perfilándolo.
                // Debemos decrementar el contador para desbloquear la tarea principal.
                if (!failed_device->is_known)
                {
                    DECR_ACTIVE_OPS();
                }
                update_device_state(failed_device->addr, BLE_DEVICE_STATE_ERROR);
            }
            else
            {
                ESP_LOGE(TAG, "No se pudo encontrar el dispositivo del intento de conexión fallido en la lista.");
                // Aún así, decrementar el contador para evitar un bloqueo permanente
                DECR_ACTIVE_OPS();
            }
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "🔐 Evento de Encriptación Cambiada (status: %d)", event->enc_change.status);
        if (event->enc_change.status == 0)
        {
            ESP_LOGW(TAG, "✅ Enlace seguro establecido (bonded).");
        }
        else
        {
            ESP_LOGE(TAG, "Error en auto-pairing/bonding: %d para conn_handle %d", event->enc_change.status, event->enc_change.conn_handle);
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Dispositivo desconectado (conn_handle: %d, reason: 0x%x)",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);

        // Limpiar contexto de discovery si lo hubiera
        cleanup_discovery_context(event->disconnect.conn.conn_handle);

        // Buscar dispositivo por el handle de la conexión que se cerró
        device = find_device_by_conn_handle(event->disconnect.conn.conn_handle);

        if (device)
        {
            ESP_LOGI(TAG, "Dispositivo '%s' desconectado", device->name);

            // Si el dispositivo no era conocido, estábamos perfilándolo. La operación ha terminado.
            if (!device->is_known)
            {
                DECR_ACTIVE_OPS();
            }

            // --- LA CORRECCIÓN CLAVE ---
            // Solo cambiar a DISCONNECTED si el dispositivo NO está ya en un estado final.
            if (device->state != BLE_DEVICE_STATE_ERROR && device->state != BLE_DEVICE_STATE_DISCOVERY_COMPLETE)
            {
                update_device_state(device->addr, BLE_DEVICE_STATE_DISCONNECTED);
            }
            else
            {
                ESP_LOGI(TAG, "Manteniendo estado final (%d) para '%s' tras desconexión.", device->state, device->name);
                // Opcionalmente, aquí también podemos limpiar el handle para asegurar consistencia
                if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
                    xSemaphoreGive(devices_mutex);
                }
            }
            // --- FIN DE LA CORRECCIÓN ---

            if (device_callbacks.on_device_disconnected)
            {
                device_callbacks.on_device_disconnected(device);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/**
 * @brief Update device state
 * @param addr Device address
 * @param new_state New state to set
 * Esta función actualiza el estado de un dispositivo BLE en la lista de dispositivos descubiertos.
 */
static void update_device_state(uint8_t addr[6], ble_device_state_t new_state)
{
    bool device_found = false;
    uint8_t addr_copy[6];                    // Copia segura de la dirección MAC
    char name_copy[BLE_DEVICE_MAX_NAME_LEN]; // Copia segura del nombre, usando la constante de tamaño
    ble_device_state_t old_state;            // Para logging del cambio

    ble_device_info_t *device = find_device_by_addr_internal(addr);
    if (!device)
    {
        ESP_LOGE(TAG, "update_device_state: dispositivo no encontrado");
        return;
    }

    // Tomar mutex
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Timeout actualizando estado del dispositivo");
        return;
    }

    // Verificar si hay dispositivos (optimización temprana para evitar bucle innecesario)
    if (discovered_count == 0)
    {
        ESP_LOGW(TAG, "No hay dispositivos descubiertos para actualizar");
        xSemaphoreGive(devices_mutex);
        return;
    }

    // Buscar y actualizar dentro del mutex
    for (int i = 0; i < discovered_count; i++)
    {
        if (memcmp(discovered_devices[i].addr, addr, 6) == 0)
        {
            old_state = discovered_devices[i].state;
            discovered_devices[i].state = new_state;

            // Actualizar last_seen si el estado cambia (para consistencia con add_or_update_discovered_device)
            discovered_devices[i].last_seen = xTaskGetTickCount();

            // Validar consistencia (asumiendo que devuelve bool; ajusta si no)
            force_consistent_state(&discovered_devices[i]); // Removí el chequeo de retorno por simplicidad, ya que el original no lo maneja; agrega si force_consistent_state devuelve valor

            // Copiar datos críticos para uso post-mutex
            memcpy(addr_copy, discovered_devices[i].addr, 6);                  // Copia de addr del dispositivo (más preciso que copiar input addr)
            strlcpy(name_copy, discovered_devices[i].name, sizeof(name_copy)); // Seguro contra NULL o overflow
            device_found = true;
            break;
        }
    }

    // Liberar mutex lo antes posible
    xSemaphoreGive(devices_mutex);

    // Registrar si no se encontró (usando formato de addr input, ya que no hay copia aún)
    if (!device_found)
    {
        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        ESP_LOGW(TAG, "Dispositivo con dirección %s no encontrado", addr_str);
        return;
    }

    // Log del cambio de estado (usando copias seguras; nivel INFO o DEBUG según necesidades)
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr_copy[0], addr_copy[1], addr_copy[2], addr_copy[3], addr_copy[4], addr_copy[5]);
    ESP_LOGI(TAG, "Estado del dispositivo %s (%s): %d → %d",
             name_copy, addr_str, old_state, new_state); // Cambié a LOGI como en original para visibilidad

    if (new_state == BLE_DEVICE_STATE_DISCOVERY_COMPLETE)
    {
        ESP_LOGW(TAG, "✅ Perfil completo para '%s'. Listo para guardar en NVS.", name_copy);
        // Marcar como conocido en la lista de memoria
        device->is_known = true;
        // Aquí es donde, en el futuro, llamaremos a la función para guardar en NVS.
        // Por ahora, nos desconectamos para poder escanear otros dispositivos.
        ble_device_disconnect(addr_copy);
    }
    else if (new_state == BLE_DEVICE_STATE_ERROR && old_state >= BLE_DEVICE_STATE_CONNECTED)
    {
        ESP_LOGW(TAG, "❌ Ocurrió un error de perfilado para '%s'. Desconectando.", name_copy);
        ble_device_disconnect(addr_copy);
    }

    // Si vamos a pasar a DISCONNECTED, limpiar handles y flags
    if (new_state == BLE_DEVICE_STATE_DISCONNECTED)
    {
        device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        device->char_val_handle = 0;
        device->pairing_char_handle = 0;
        device->pairing_char_found = false;
        device->control_char_found = false;
        device->char_discovered = false;
        device->matched_profile_index = -1;
        memset(&device->service_uuid_128, 0, sizeof(device->service_uuid_128));
        memset(&device->char_uuid_128, 0, sizeof(device->char_uuid_128));
        ESP_LOGI(TAG, "Limpieza completa de handles y flags para dispositivo '%s'", name_copy);
    }
}

/**
 * @brief Add discovered device to list
 * @param scanned_device Device information to add or update
 * This function adds a discovered BLE device to the list of known devices.
 */
static esp_err_t add_or_update_discovered_device(ble_device_info_t *scanned_device)
{
    if (devices_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Buscar si ya existe en la lista (cargado de NVS o de un escaneo previo)
    ble_device_info_t *existing_device = NULL;
    for (int i = 0; i < discovered_count; i++)
    {
        if (memcmp(discovered_devices[i].addr, scanned_device->addr, 6) == 0)
        {
            existing_device = &discovered_devices[i];
            break;
        }
    }

    if (existing_device)
    {
        // Ya lo conocemos, solo actualizamos datos volátiles
        existing_device->rssi = scanned_device->rssi;
        existing_device->last_seen = xTaskGetTickCount();
        // Opcional: Actualizar nombre si ha cambiado
        if (strlen(scanned_device->name) > 0 && strcmp(existing_device->name, scanned_device->name) != 0)
        {
            strncpy(existing_device->name, scanned_device->name, BLE_DEVICE_MAX_NAME_LEN - 1);
        }
        if (scanned_device->type != BLE_DEVICE_TYPE_UNKNOWN)
        {
            existing_device->type = scanned_device->type;
        }
    }
    else
    {
        // Es un dispositivo completamente nuevo, lo añadimos
        if (discovered_count < BLE_DEVICE_MAX_DEVICES)
        {
            memcpy(&discovered_devices[discovered_count], scanned_device, sizeof(ble_device_info_t));
            discovered_devices[discovered_count].is_known = false;        // Marcar como nuevo
            discovered_devices[discovered_count].char_discovered = false; // No hemos completado discovery aún
            discovered_devices[discovered_count].state = BLE_DEVICE_STATE_DISCONNECTED;
            discovered_devices[discovered_count].matched_profile_index = -1;
            discovered_count++;

            // === Actualizar métricas ===
            discovery_metrics.nuevos_dispositivos_en_ciclo++;
            discovery_metrics.tiempo_ultimo_nuevo = xTaskGetTickCount();
        }
        else
        {
            xSemaphoreGive(devices_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreGive(devices_mutex);
    return ESP_OK;
}

/**
 * @brief Find device by connection handle
 * @param conn_handle Connection handle to search for
 * @return Pointer to the device info structure if found, NULL otherwise
 * This function searches for a device in the discovered devices list by its connection handle.
 */
static ble_device_info_t *find_device_by_conn_handle(uint16_t conn_handle)
{
    ble_device_info_t *found_device = NULL;

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || devices_mutex == NULL)
    {
        return NULL;
    }

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 0; i < discovered_count; i++)
        {
            if (discovered_devices[i].conn_handle == conn_handle)
            {
                found_device = &discovered_devices[i];
                break;
            }
        }
        xSemaphoreGive(devices_mutex);
    }

    return found_device;
}

/**
 * @brief Find device by address (internal)
 * @param addr Address of the device to find
 * @return Pointer to the device info structure if found, NULL otherwise
 * This function searches for a device in the discovered devices list by its address.
 */
// Versión GROK
static ble_device_info_t *find_device_by_addr_internal(uint8_t addr[6])
{
    ble_device_info_t *ret = NULL;

    // Manejar caso de mutex no inicializado (fallar de manera segura en lugar de buscar sin protección)
    if (devices_mutex == NULL)
    {
        ESP_LOGE(TAG, "Mutex de dispositivos no inicializado");
        return NULL;
    }

    // Tomar mutex con timeout consistente (alineado con otras funciones)
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Timeout buscando dispositivo por addr");
        return NULL;
    }

    // Verificación temprana para evitar bucle innecesario
    if (discovered_count == 0)
    {
        xSemaphoreGive(devices_mutex);
        return NULL;
    }

    // Buscar el dispositivo
    for (int i = 0; i < discovered_count; i++)
    {
        if (memcmp(discovered_devices[i].addr, addr, 6) == 0)
        {
            ret = &discovered_devices[i];
            xSemaphoreGive(devices_mutex); // Liberar mutex tempranamente para minimizar bloqueo
            return ret;
        }
    }

    // No encontrado: liberar mutex y retornar NULL
    xSemaphoreGive(devices_mutex);
    return NULL;
}

/**
 * @brief Disconnect from a BLE device
 * @param device_addr Address of the device to disconnect
 * @return ESP_OK on success, error code on failure
 * Esta función desconecta un dispositivo BLE dado su dirección.
 */
esp_err_t ble_device_disconnect(uint8_t device_addr[6])
{
    if (!module_initialized || module_stopping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Buscar dispositivo en la lista
    ble_device_info_t *device = find_device_by_addr_internal(device_addr);
    if (!device)
    {
        ESP_LOGE(TAG, "Dispositivo no encontrado para desconectar");
        return ESP_ERR_NOT_FOUND;
    }

    // Si ya está desconectado, nada que hacer
    if (device->state == BLE_DEVICE_STATE_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Dispositivo %s ya está desconectado", device->name);
        return ESP_OK;
    }

    // Si no tenemos connection handle válido, marcar desconectado localmente y salir
    if (device->conn_handle == BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGW(TAG, "No hay connection handle válido para %s (estado=%d). Normalizando a DISCONNECTED.",
                 device->name, device->state);
        update_device_state(device->addr, BLE_DEVICE_STATE_DISCONNECTED);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Desconectando dispositivo: %s (handle=%d, estado=%d)",
             device->name ? device->name : "(sin nombre)", device->conn_handle, device->state);

    int rc = ble_gap_terminate(device->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error ble_gap_terminate() desconectando %s: %d", device->name, rc);
        // Si devuelve que no estaba conectado (o invalido), limpiar localmente
        if (rc == BLE_HS_ENOTCONN || rc == BLE_HS_EINVAL)
        {
            update_device_state(device->addr, BLE_DEVICE_STATE_DISCONNECTED);
            device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            return ESP_OK;
        }
        return ESP_FAIL;
    }

    // Éxito: el evento de desconexión llegará al callback y ahí se limpiará g_active_ble_operations.
    return ESP_OK;
}

/**
 * @brief Get count of discovered devices
 * @return Number of discovered devices
 * Esta función obtiene el conteo de dispositivos BLE descubiertos.
 */
int ble_device_get_discovered_count(void)
{
    int count = 0;
    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        count = discovered_count;
        xSemaphoreGive(devices_mutex);
    }
    return count;
}

/**
 * @brief Get list of discovered devices
 * @param devices Array to fill with discovered devices
 * @param max_devices Maximum number of devices to return
 * @return Number of devices filled in the array, or 0 on error
 * Esta función obtiene la lista de dispositivos BLE descubiertos.
 */
int ble_device_get_discovered_list(ble_device_info_t devices[], int max_devices)
{
    if (!devices || max_devices <= 0 || !module_initialized)
    {
        return 0;
    }

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Timeout obteniendo lista de dispositivos");
        return 0;
    }

    int count = MIN(discovered_count, max_devices);

    // Copiar dispositivos descubiertos
    for (int i = 0; i < count; i++)
    {
        memcpy(&devices[i], &discovered_devices[i], sizeof(ble_device_info_t));
    }

    xSemaphoreGive(devices_mutex);

    ESP_LOGI(TAG, "Devolviendo %d dispositivos de %d total", count, discovered_count);
    return count;
}

/**
 * @brief Find device by address (public API)
 * @param addr Address of the device to find
 * @return Pointer to the device info structure if found, NULL otherwise
 * Esta función busca un dispositivo BLE en la lista de dispositivos descubiertos por su dirección.
 */
ble_device_info_t *ble_device_find_by_addr(uint8_t addr[6])
{
    if (!addr || !module_initialized)
    {
        return NULL;
    }

    // Usar la función interna que ya existe
    return find_device_by_addr_internal(addr);
}

/**
 * @brief Find device by name (public API)
 * @param name Name of the device to find
 * @return Pointer to the device info structure if found, NULL otherwise
 * Esta función busca un dispositivo BLE en la lista de dispositivos descubiertos por su nombre.
 */
ble_device_info_t *ble_device_find_by_name(const char *name)
{
    if (!name || !module_initialized)
    {
        return NULL;
    }

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return NULL;

    ble_device_info_t *found_device = NULL;
    for (int i = 0; i < discovered_count; i++)
    {
        if (strstr(discovered_devices[i].name, name) != NULL)
        {
            found_device = &discovered_devices[i];
            break; // Salir del loop sin retornar
        }
    }

    xSemaphoreGive(devices_mutex); // SIEMPRE liberar mutex
    return found_device;
}

/**
 * @brief Detect BLE device type from name
 * @param name Name of the device
 * @return Detected device type, or BLE_DEVICE_TYPE_UNKNOWN if not recognized
 * Esta función detecta el tipo de dispositivo BLE basado en su nombre.
 */
ble_device_type_t ble_device_detect_type_from_name(const char *name)
{
    if (!name)
    {
        return BLE_DEVICE_TYPE_UNKNOWN;
    }

    // Convertir a minúsculas para comparación
    char lower_name[BLE_DEVICE_MAX_NAME_LEN];
    size_t name_len = strlen(name);
    size_t copy_len = (name_len < sizeof(lower_name) - 1) ? name_len : sizeof(lower_name) - 1;
    for (int i = 0; i < copy_len; i++)
    {
        lower_name[i] = tolower(name[i]);
    }
    lower_name[MIN(strlen(name), sizeof(lower_name) - 1)] = '\0';

    // Detectar tipos basados en palabras clave
    if (strstr(lower_name, "light") || strstr(lower_name, "lamp") ||
        strstr(lower_name, "bulb") || strstr(lower_name, "led") ||
        strstr(lower_name, "luz") || strstr(lower_name, "lámpara") ||
        strstr(lower_name, "bathroom") || strstr(lower_name, "baño") || strstr(lower_name, "pasillo"))
    {
        return BLE_DEVICE_TYPE_LIGHT;
    }
    if (strstr(lower_name, "fan") || strstr(lower_name, "ventilador"))
    {
        return BLE_DEVICE_TYPE_FAN;
    }
    if (strstr(lower_name, "vacuum") || strstr(lower_name, "aspiradora"))
    {
        return BLE_DEVICE_TYPE_VACUUM;
    }
    if (strstr(lower_name, "speaker") || strstr(lower_name, "audio"))
    {
        return BLE_DEVICE_TYPE_SPEAKER;
    }
    if (strstr(lower_name, "thermostat") || strstr(lower_name, "temp"))
    {
        return BLE_DEVICE_TYPE_THERMOSTAT;
    }

    return BLE_DEVICE_TYPE_UNKNOWN;
}

/**
 * @brief Check if a service UUID is in known profiles
 */
static bool is_service_in_known_profiles(const ble_uuid_t *uuid, int *out_profile_index)
{
    if (uuid->type != BLE_UUID_TYPE_128)
        return false;

    // Buscar en el array global cargado desde NVS
    for (int i = 0; i < g_known_profiles_count; i++)
    {
        // Acceder al campo .u de ble_uuid128_t para obtener ble_uuid_t
        if (ble_uuid_cmp(&g_known_profiles[i].service_uuid.u, uuid) == 0)
        {
            if (out_profile_index)
                *out_profile_index = i;
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if a characteristic UUID is in known profiles
 */
static bool is_characteristic_in_known_profiles(const ble_uuid_t *uuid, int profile_index)
{
    if (uuid->type != BLE_UUID_TYPE_128 || profile_index < 0 || profile_index >= g_known_profiles_count)
    {
        return false;
    }

    // Comparamos solo con las características del perfil que ya coincidió
    const known_device_profile_t *profile = &g_known_profiles[profile_index];
    for (int i = 0; i < profile->num_characteristics; i++)
    {
        // Cast ble_uuid128_t* a ble_uuid_t* usando la dirección del campo base
        if (ble_uuid_cmp(&profile->characteristics[i].uuid.u, uuid) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Callback for characteristic discovery
 * @param conn_handle Connection handle
 * @param error Error information
 * @param chr Characteristic that was discovered
 * @param arg Argument passed to the callback
 * Esta función se llama cuando se descubre una característica durante el descubrimiento de servicios.
 */
static int on_characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                        const struct ble_gatt_chr *chr, void *arg)
{
    ble_device_info_t *device = (ble_device_info_t *)arg;
    if (!device)
        return 0;

    if (error->status == BLE_HS_EDONE)
    {
        ESP_LOGI(TAG, "Descubrimiento de características completo para el servicio actual.");
        // Si encontramos al menos una característica de interés, marcamos el perfil como completo.
        if (device->char_discovered)
        {
            ESP_LOGW(TAG, "✅ Perfil del dispositivo '%s' aprendido exitosamente.", device->name);
            update_device_state(device->addr, BLE_DEVICE_STATE_DISCOVERY_COMPLETE);
        }
        else
        {
            ESP_LOGW(TAG, "No se encontraron características de perfiles conocidos para '%s'", device->name);
            mark_device_as_error_and_disconnect(device);
        }
        return 0;
    }
    if (error->status != 0)
    { /* ... manejo de error ... */
        return 0;
    }

    char uuid_str[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&chr->uuid.u, uuid_str);
    ESP_LOGI(TAG, "    -> Característica encontrada: %s", uuid_str);

    // Comparamos la característica con nuestra base de datos de perfiles
    if (is_characteristic_in_known_profiles(&chr->uuid.u, device->matched_profile_index))
    {
        ESP_LOGW(TAG, "  ✅ Característica '%s' coincide con un perfil conocido.", uuid_str);
        device->char_discovered = true; // Marcamos que encontramos al menos una

        // Guardamos la primera característica de interés que encontremos
        if (device->char_uuid_128.u.type == 0)
        {
            memcpy(&device->char_uuid_128, &chr->uuid.u128, sizeof(ble_uuid128_t));
            device->char_val_handle = chr->val_handle;
        }
    }
    return 0;
}

static int on_service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service, void *arg)
{
    ble_device_info_t *device = (ble_device_info_t *)arg;
    if (!device)
        return 0;

    if (error->status == BLE_HS_EDONE)
    {
        ESP_LOGI(TAG, "Descubrimiento de servicios completo para '%s'.", device->name);
        // Si no encontramos ningún servicio de interés, marcamos como error.
        if (device->state < BLE_DEVICE_STATE_DISCOVERING_CHRS)
        {
            ESP_LOGW(TAG, "No se encontraron servicios de perfiles conocidos para '%s'", device->name);
            mark_device_as_error_and_disconnect(device);
        }
        return 0;
    }
    if (error->status != 0)
    { /* ... manejo de error ... */
        return 0;
    }

    char uuid_str[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&service->uuid.u, uuid_str);
    ESP_LOGI(TAG, "  -> Servicio encontrado: %s", uuid_str);

    // Comparamos el servicio con nuestra base de datos de perfiles
    int profile_index = -1;
    if (is_service_in_known_profiles(&service->uuid.u, &profile_index))
    {
        ESP_LOGW(TAG, "✅ Servicio '%s' coincide con un perfil conocido. Descubriendo sus características...", uuid_str);
        device->matched_profile_index = profile_index; // <-- Guardamos el índice

        update_device_state(device->addr, BLE_DEVICE_STATE_DISCOVERING_CHRS);

        // Guardamos el primer servicio de interés que encontremos
        if (device->service_uuid_128.u.type == 0)
        { // Guardar solo si no hemos guardado uno ya
            memcpy(&device->service_uuid_128, &service->uuid.u128, sizeof(ble_uuid128_t));
        }

        ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                on_characteristic_discovered, device);
        return 1; // Detener la búsqueda de más servicios
    }
    return 0;
}

/**
 * @brief Cleanup discovery context for a connection handle
 * @param conn_handle Connection handle to clean up
 * Esta función limpia el contexto de descubrimiento para un handle de conexión específico.
 */
// Esta función es crítica para liberar recursos y evitar fugas de memoria.
static void cleanup_discovery_context(uint16_t conn_handle)
{
    if (devices_mutex == NULL)
    {
        return;
    }

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        for (int i = 0; i < BLE_DEVICE_MAX_DEVICES; i++)
        {
            if (discovery_contexts[i].conn_handle == conn_handle && discovery_contexts[i].in_use)
            {
                // Agregar flag para evitar double-cleanup
                if (!discovery_contexts[i].in_use)
                {
                    ESP_LOGD(TAG, "Contexto ya limpiado para conn_handle: %d", conn_handle);
                    xSemaphoreGive(devices_mutex);
                    return;
                }

                discovery_contexts[i].conn_handle = 0;
                discovery_contexts[i].services_discovered = false;
                discovery_contexts[i].characteristics_discovered = false;
                discovery_contexts[i].in_use = false;
                if (active_discoveries > 0)
                {
                    active_discoveries--;
                }
                ESP_LOGD(TAG, "Contexto de descubrimiento limpiado para conn_handle: %d", conn_handle);
                break;
            }
        }
        xSemaphoreGive(devices_mutex);
    }
}

/**
 * @brief Habilita o deshabilita la auto-conexión globalmente
 * @param enable Verdadero para habilitar, falso para deshabilitar
 * Esta función permite habilitar o deshabilitar la auto-conexión de dispositivos BLE.
 */
void ble_device_set_auto_connection_enabled(bool enable)
{
    auto_connection_globally_enabled = enable;
    ESP_LOGI(TAG, "Auto-conexión %s", enable ? "HABILITADA" : "DESHABILITADA");
}

/**
 * @brief Inicia descubrimiento inteligente con auto-conexión
 * @return ESP_OK on success, error code on failure
 * Esta función inicia el descubrimiento inteligente de dispositivos BLE con auto-conexión.
 */
esp_err_t ble_device_start_smart_discovery(uint32_t duration_ms)
{
    if (!ble_central_diagnostic_enabled())
    {
        ESP_LOGW(TAG, "Smart discovery rejected; BLE Central diagnostic is disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Módulo no está inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "🚀 INICIANDO DESCUBRIMIENTO INTELIGENTE DE DISPOSITIVOS (%lu ms)", duration_ms);

    if (!auto_connection_globally_enabled)
    {
        ESP_LOGW(TAG, "⚠️ Auto-conexión está deshabilitada globalmente");
    }

    // Usar la duración pasada como parámetro en lugar de un valor fijo
    esp_err_t ret = ble_device_start_scan(duration_ms);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "✅ Escaneo inteligente iniciado por %lu segundos", duration_ms / 1000);
        if (auto_connection_globally_enabled)
        {
            ESP_LOGI(TAG, "💡 Los dispositivos de interés se conectarán automáticamente");
        }
    }
    else
    {
        ESP_LOGE(TAG, "❌ Error iniciando escaneo inteligente: %d", ret);
    }

    return ret;
}
/**
 * @brief Configura parámetros del descubrimiento inteligente
 * @param normal_interval_ms Intervalo normal de escaneo en milisegundos
 * @param maintenance_interval_ms Intervalo de mantenimiento en milisegundos
 * @param target_devices Número de dispositivos objetivo a descubrir
 * @param enable_test_commands Habilita comandos de prueba para focos
 * @return ESP_OK on success, error code on failure
 * Esta función configura los parámetros del descubrimiento inteligente de dispositivos BLE.
 */
static bool ble_identity_adv_has_target_uuid(const struct ble_hs_adv_fields *fields)
{
    if (fields == NULL) return false;

    // Define acceptable identities
    ble_uuid16_t primary_uuid16 = BLE_UUID16_INIT(0xAAAA);

    // Check 16-bit UUIDs
    for (int i = 0; i < fields->num_uuids16; i++)
    {
        if (ble_uuid_cmp(&fields->uuids16[i].u, &primary_uuid16.u) == 0)
        {
            return true;
        }
    }

    // Fallback: Check 32-bit/128-bit if 16-bit doesn't match
    for (int i = 0; i < fields->num_uuids32; i++)
    {
        if (ble_uuid_cmp(&fields->uuids32[i].u, &s_identity_service_uuid.u) == 0)
        {
            return true;
        }
    }

    // 3. Fallback for 128-bit
    for (int i = 0; i < fields->num_uuids128; i++)
    {
        if (ble_uuid_cmp(&fields->uuids128[i].u, &s_identity_service_uuid.u) == 0)
        {
            return true;
        }
    }

    return false;
}

static void ble_identity_validation_reset(uint32_t timeout_ms)
{
    if (identity_validation_mutex != NULL &&
        xSemaphoreTake(identity_validation_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        identity_validation_state.timeout_ms = timeout_ms;
        identity_validation_state.seen_uuid = false;
        identity_validation_state.present = false;
        identity_validation_state.cancel_requested = false;
        identity_validation_state.best_rssi = -127;
        identity_validation_state.last_rssi = -127;
        identity_validation_state.validated_name[0] = '\0';
        xSemaphoreGive(identity_validation_mutex);
    }
}

static const char* get_identity_name_from_uuid(const char* uuid_str)
{
    if (strcmp(uuid_str, BLE_IDENTITY_SERVICE_UUID_STR) == 0) {
        return "Lorenzo";
    }
    return "Desconocido";
}

static bool ble_identity_validation_record_match(int8_t rssi, const char* name)
{
    bool mutex_taken = false;
    bool request_cancel = false;

    if (identity_validation_mutex == NULL)
    {
        ESP_LOGE(TAG, "Mutex is NULL, recording identity match without lock!");
    }
    else if (xSemaphoreTake(identity_validation_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Mutex timeout! Recording identity match without lock.");
    }
    else
    {
        mutex_taken = true;
    }

    identity_validation_state.seen_uuid = true;
    identity_validation_state.last_rssi = rssi;
    if (name) {
        strncpy(identity_validation_state.validated_name, name, sizeof(identity_validation_state.validated_name) - 1);
        identity_validation_state.validated_name[sizeof(identity_validation_state.validated_name) - 1] = '\0';
    }
    if (rssi > identity_validation_state.best_rssi)
    {
        identity_validation_state.best_rssi = rssi;
    }

    identity_validation_state.present = true;
    if (!identity_validation_state.cancel_requested)
    {
        identity_validation_state.cancel_requested = true;
        request_cancel = true;
    }

    ESP_LOGI(TAG, "Identity UUID observed without RSSI gate: rssi=%d best=%d present=%d",
             rssi,
             identity_validation_state.best_rssi,
             identity_validation_state.present);

    if (mutex_taken)
    {
        xSemaphoreGive(identity_validation_mutex);
        ESP_LOGI(TAG, "Identity match recorded successfully!");
    }
    else
    {
        ESP_LOGE(TAG, "Identity match recorded without mutex; validation state may be racy.");
    }

    return request_cancel;
}

static bool ble_identity_validation_is_present(int8_t *best_rssi, int8_t *last_rssi, bool *seen_uuid)
{
    bool present = false;

    if (identity_validation_mutex != NULL &&
        xSemaphoreTake(identity_validation_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        present = identity_validation_state.present;
        if (best_rssi != NULL)
        {
            *best_rssi = identity_validation_state.best_rssi;
        }
        if (last_rssi != NULL)
        {
            *last_rssi = identity_validation_state.last_rssi;
        }
        if (seen_uuid != NULL)
        {
            *seen_uuid = identity_validation_state.seen_uuid;
        }
        xSemaphoreGive(identity_validation_mutex);
    }

    return present;
}

const char *ble_identity_get_last_validated_name(void)
{
    static char name_copy[32];
    name_copy[0] = '\0';

    if (identity_validation_mutex != NULL &&
        xSemaphoreTake(identity_validation_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        strncpy(name_copy, identity_validation_state.validated_name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        xSemaphoreGive(identity_validation_mutex);
    }

    return name_copy;
}

static void ble_identity_validation_cancel_scan_early(void)
{
    int rc = ble_gap_disc_cancel();
    if (rc == 0)
    {
        ESP_LOGI(TAG, "Identity match accepted; GAP discovery cancel requested for early validation exit");
        if (scan_complete_semaphore != NULL)
        {
            xSemaphoreGive(scan_complete_semaphore);
        }
        return;
    }

    if (!ble_device_gap_discovery_active())
    {
        ESP_LOGI(TAG, "Identity early-exit cancel returned %d; GAP already idle", rc);
        scanning_active = false;
        if (scan_complete_semaphore != NULL)
        {
            xSemaphoreGive(scan_complete_semaphore);
        }
        return;
    }

    ESP_LOGW(TAG, "Identity early-exit cancel returned %d while GAP remains active", rc);
}

static int ble_gap_identity_validation_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event == NULL)
    {
        return 0;
    }

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
    {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0)
        {
            return 0;
        }

        if (!ble_identity_adv_has_target_uuid(&fields))
        {
            return 0;
        }

        ESP_LOGI(TAG, "Identity service UUID %s observed at RSSI=%d dBm",
                 BLE_IDENTITY_SERVICE_UUID_STR,
                 event->disc.rssi);
        const char *name = get_identity_name_from_uuid(BLE_IDENTITY_SERVICE_UUID_STR);
        if (ble_identity_validation_record_match(event->disc.rssi, name))
        {
            ble_identity_validation_cancel_scan_early();
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Identity validation scan completed");
        scanning_active = false;
        if (scan_complete_semaphore != NULL)
        {
            xSemaphoreGive(scan_complete_semaphore);
        }
        break;

    default:
        break;
    }

    return 0;
}

static esp_err_t ble_device_start_identity_scan(uint32_t timeout_ms)
{
    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Identity validation rejected; BLE device control is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (scanning_active)
    {
        ESP_LOGW(TAG, "Identity validation rejected; scan already active");
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_gap_disc_params disc_params = {
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .itvl = 0x30,
        .window = 0x30,
        .filter_duplicates = 0,
    };

    scanning_active = true;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          timeout_ms == 0 ? BLE_HS_FOREVER : timeout_ms,
                          &disc_params,
                          ble_gap_identity_validation_event_handler,
                          NULL);
    if (rc != 0)
    {
        scanning_active = false;
        ESP_LOGE(TAG, "Error starting identity validation passive scan: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Identity validation passive scan started for %lu ms; UUID=%s",
             timeout_ms,
             BLE_IDENTITY_SERVICE_UUID_STR);
    return ESP_OK;
}

static void ble_device_run_identity_validation(void)
{
    const uint32_t timeout_ms = identity_validation_state.timeout_ms;
    bool scan_completed = false;

    ESP_LOGI(TAG, "Starting bounded BLE identity validation window (%lu ms)", timeout_ms);

    ble_identity_validation_reset(timeout_ms);

    ble_device_drain_scan_complete_signal();

    esp_err_t err = ble_device_start_identity_scan(timeout_ms);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Identity validation scan failed to start: %s", esp_err_to_name(err));
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        ble_common_release(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);
        return;
    }

    if (scan_complete_semaphore != NULL)
    {
        scan_completed = xSemaphoreTake(scan_complete_semaphore,
                                        pdMS_TO_TICKS(timeout_ms + 1000)) == pdTRUE;
    }

    if (!scan_completed)
    {
        ESP_LOGW(TAG, "Identity validation scan timed out locally; cancelling GAP discovery");
        ble_device_stop_scan();
    }

    int8_t best_rssi = -127;
    int8_t last_rssi = -127;
    bool seen_uuid = false;
    bool present = ble_identity_validation_is_present(&best_rssi, &last_rssi, &seen_uuid);

    if (scan_completed && present)
    {
        if (!ble_device_gap_discovery_active())
        {
            scanning_active = false;
            ESP_LOGI(TAG, "Identity early exit completed; GAP discovery already idle");
        }
        else
        {
            bool cancel_acknowledged = false;
            if (scan_complete_semaphore != NULL)
            {
                cancel_acknowledged =
                    xSemaphoreTake(scan_complete_semaphore,
                                   pdMS_TO_TICKS(IDENTITY_EARLY_EXIT_CANCEL_WAIT_MS)) == pdTRUE;
            }

            if (cancel_acknowledged || !ble_device_gap_discovery_active())
            {
                scanning_active = false;
                ESP_LOGI(TAG, "Identity early exit completed; GAP discovery idle");
            }
            else
            {
                ESP_LOGW(TAG, "Identity early exit woke before GAP idle; forcing scan stop");
                ble_device_stop_scan();
            }
        }
    }

    if (seen_uuid && present)
    {
        ESP_LOGI(TAG, "Identity validated: uuid=%s best=%d last=%d",
                 BLE_IDENTITY_SERVICE_UUID_STR,
                 best_rssi,
                 last_rssi);
        orchestrator_post_event(ORCH_EVENT_IDENTITY_PRESENT);
    }
    else
    {
        ESP_LOGW(TAG, "Identity rejected: seen_uuid=%d present=%d best=%d last=%d",
                 seen_uuid,
                 present,
                 best_rssi,
                 last_rssi);
        orchestrator_post_event(ORCH_EVENT_IDENTITY_REJECTED);
    }

    ble_common_release(BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC);
}

esp_err_t ble_device_prepare_for_identity_scan(uint32_t timeout_ms)
{
    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Identity scan prepare rejected; BLE module is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ble_common_get_owner() != BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC)
    {
        ESP_LOGE(TAG, "Identity scan prepare rejected; central role is not owned");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_ble_task_handle == NULL ||
        smart_task_start_signal == NULL ||
        smart_task_idle_signal == NULL)
    {
        ESP_LOGE(TAG, "Identity scan prepare rejected; smart BLE task resources are not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_discovery_running || smart_discovery_enabled)
    {
        ESP_LOGE(TAG, "Identity scan prepare rejected; smart BLE task is active");
        return ESP_ERR_INVALID_STATE;
    }

    if (scanning_active || ble_device_gap_discovery_active())
    {
        ESP_LOGW(TAG, "Identity scan prepare found active GAP discovery; cancelling before validation");
        esp_err_t err = ble_device_cancel_scan_and_wait(timeout_ms);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    ble_device_drain_scan_complete_signal();
    ESP_LOGI(TAG, "Identity scan prepare complete; GAP discovery idle");
    return ESP_OK;
}

esp_err_t ble_device_start_identity_validation(uint32_t timeout_ms)
{
    if (timeout_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Identity validation rejected; BLE module is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ble_common_get_owner() != BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC)
    {
        ESP_LOGE(TAG, "Identity validation rejected; central role is not owned");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_ble_task_handle == NULL ||
        smart_task_start_signal == NULL ||
        smart_task_idle_signal == NULL)
    {
        ESP_LOGE(TAG, "Identity validation rejected; smart BLE task resources are not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_discovery_running || smart_discovery_enabled)
    {
        ESP_LOGW(TAG, "Identity validation rejected; smart BLE task is already active");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t prepare_err = ble_device_prepare_for_identity_scan(1000);
    if (prepare_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Identity validation rejected; BLE GAP not ready: %s",
                 esp_err_to_name(prepare_err));
        return prepare_err;
    }

    ble_identity_validation_reset(timeout_ms);
    smart_task_mode = BLE_SMART_TASK_MODE_IDENTITY_VALIDATION;

    xSemaphoreTake(smart_task_idle_signal, 0);
    xSemaphoreTake(smart_task_start_signal, 0);

    smart_discovery_enabled = true;
    smart_discovery_running = true;

    if (xSemaphoreGive(smart_task_start_signal) != pdTRUE)
    {
        smart_discovery_enabled = false;
        smart_discovery_running = false;
        smart_task_mode = BLE_SMART_TASK_MODE_DISCOVERY;
        ESP_LOGE(TAG, "Failed to wake smart BLE task for identity validation");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Identity validation scheduled for %lu ms", timeout_ms);
    return ESP_OK;
}

esp_err_t ble_device_configure_smart_discovery(uint32_t normal_interval_ms,
                                               uint32_t maintenance_interval_ms,
                                               int target_devices,
                                               bool enable_test_commands)
{
    if (normal_interval_ms < 10000 || maintenance_interval_ms < 30000)
    {
        ESP_LOGE(TAG, "❌ Intervalos muy cortos (mín: 10s normal, 30s mantenimiento)");
        return ESP_ERR_INVALID_ARG;
    }

    smart_config.scan_interval_normal_ms = normal_interval_ms;
    smart_config.scan_interval_maintenance_ms = maintenance_interval_ms;
    smart_config.target_devices = target_devices;
    smart_config.auto_test_commands = enable_test_commands;
    smart_config.max_retries = 2; // Valor fijo por ahora

    ESP_LOGI(TAG, "⚙️ Configuración actualizada - Normal: %lus, Mantenimiento: %lus, Objetivo: %d",
             normal_interval_ms / 1000, maintenance_interval_ms / 1000, target_devices);

    return ESP_OK;
}

/**
 * @brief Fuerza la limpieza de operaciones BLE bloqueadas
 * Esta función intenta limpiar cualquier operación BLE que pueda estar bloqueada o en un estado inconsistente.
 * Se utiliza como último recurso cuando se detectan operaciones atascadas.
 */
static void force_cleanup_stuck_operations(void)
{
    ESP_LOGW(TAG, "🧹 Iniciando limpieza forzada de operaciones bloqueadas...");

    if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        int cleaned_count = 0;

        for (int i = 0; i < discovered_count; i++)
        {
            ble_device_info_t *device = &discovered_devices[i];

            // Limpiar dispositivos en estados inconsistentes
            if (!device->is_known &&
                (device->state == BLE_DEVICE_STATE_CONNECTING ||
                 device->state == BLE_DEVICE_STATE_CONNECTED ||
                 device->state == BLE_DEVICE_STATE_DISCOVERING_SVCS ||
                 device->state == BLE_DEVICE_STATE_DISCOVERING_CHRS))
            {

                ESP_LOGW(TAG, "🧽 Limpiando dispositivo bloqueado: %s (estado: %d)",
                         device->name, device->state);

                // Forzar desconexión si tiene handle
                if (device->conn_handle != BLE_HS_CONN_HANDLE_NONE)
                {
                    ble_gap_terminate(device->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }

                // Limpiar estado
                device->conn_handle = BLE_HS_CONN_HANDLE_NONE;
                device->char_val_handle = 0;
                device->pairing_char_handle = 0;
                device->state = BLE_DEVICE_STATE_ERROR;

                cleaned_count++;
            }
        }

        xSemaphoreGive(devices_mutex);
        ESP_LOGW(TAG, "🧹 Limpieza completada: %d dispositivos procesados", cleaned_count);
    }

    // Resetear contador atómico a 0 como último recurso
    int old_count = atomic_exchange(&g_active_ble_operations, 0);
    ESP_LOGW(TAG, "🔄 Contador de operaciones reseteado de %d a 0", old_count);
}

/**
 * @brief Obtiene estadísticas del descubrimiento inteligente
 * @param total_cycles Puntero para almacenar el número total de ciclos de descubrimiento
 * @param successful_discoveries Puntero para almacenar el número de descubrimientos exitosos
 * @param failed_discoveries Puntero para almacenar el número de descubrimientos fallidos
 * @param is_running Puntero para indicar si el descubrimiento inteligente está en ejecución
 * @return ESP_OK on success, error code on failure
 * Esta función obtiene las estadísticas del descubrimiento inteligente de dispositivos BLE.
 */
static void smart_ble_discovery_btdevices_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Tarea persistente de Agente de Descubrimiento creada.");

    while (true)
    {
        if (xSemaphoreTake(smart_task_start_signal, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        while (xSemaphoreTake(smart_task_start_signal, 0) == pdTRUE)
        {
            /* Collapse duplicate start signals. */
        }

        if (smart_task_shutdown_requested)
        {
            break;
        }

        if (!module_initialized || module_stopping || !smart_discovery_enabled)
        {
            smart_discovery_running = false;
            if (smart_task_idle_signal != NULL)
            {
                xSemaphoreGive(smart_task_idle_signal);
            }
            continue;
        }

        ESP_LOGI(TAG, "Tarea de Agente de Descubrimiento activada.");

        ESP_LOGI(TAG, "🤖 Tarea de Agente de Descubrimiento iniciada.");
        smart_discovery_running = true;

        if (smart_task_mode == BLE_SMART_TASK_MODE_IDENTITY_VALIDATION)
        {
            ble_device_run_identity_validation();
            goto task_exit;
        }

        if (!ble_smart_delay_or_stopped(5000))
        {
            goto task_exit;
        }

        uint32_t consecutive_failures = 0;
        const uint32_t MAX_CONSECUTIVE_FAILURES = 5;

        while (smart_discovery_enabled && !module_stopping)
        {
            size_t free_heap = esp_get_free_heap_size();
            if (free_heap < MIN_HEAP_BYTES * 2)
            { // Buffer más conservador
                ESP_LOGW(TAG, "🛑 Memoria insuficiente para ciclo BLE (%zu bytes), pausando...", free_heap);
                if (!ble_smart_delay_or_stopped(30000))
                {
                    break;
                }
                continue;
            }
            // --- INICIO DE LA CORRECCIÓN: RESETEO INTELIGENTE DE LA LISTA ---
            ESP_LOGI(TAG, "Limpiando lista de dispositivos transitorios para nuevo ciclo...");
            if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
            {

                int next_free_slot = 0;
                // Recorremos la lista actual
                for (int i = 0; i < discovered_count; i++)
                {
                    // Mantenemos solo los dispositivos que son CONOCIDOS (de NVS)
                    // O los que han terminado en un estado de ERROR (nuestra "lista negra")
                    if (discovered_devices[i].is_known || discovered_devices[i].state == BLE_DEVICE_STATE_ERROR)
                    {
                        // Si el dispositivo que queremos mantener no está ya en su posición final, lo movemos.
                        if (i != next_free_slot)
                        {
                            memcpy(&discovered_devices[next_free_slot], &discovered_devices[i], sizeof(ble_device_info_t));
                        }
                        next_free_slot++;
                    }
                }

                // Actualizamos el contador al número de dispositivos que hemos conservado
                discovered_count = next_free_slot;
                ESP_LOGI(TAG, "Limpieza completada. %d dispositivos persistentes conservados.", discovered_count);

                xSemaphoreGive(devices_mutex);
            }
            // --- FIN DE LA CORRECCIÓN ---

            memset(&best_candidate, 0, sizeof(best_candidate));
            best_candidate.rssi = -127;

            smart_stats.total_cycles++;
            ESP_LOGI(TAG, "🔄 === CICLO #%lu DE DESCUBRIMIENTO INTELIGENTE ===", smart_stats.total_cycles);

            if (smart_config.max_retries <= 0)
            {
                ESP_LOGE(TAG, "⌛ Error: max_retries no inicializado (%d), usando valor por defecto", smart_config.max_retries);
                smart_config.max_retries = 2;
            }

            ESP_LOGI(TAG, "🔧 Configuración actual - Reintentos máximos: %d", smart_config.max_retries);

            // 1. Lógica de Modo Mantenimiento
            if (target_devices_reached() && !smart_config.maintenance_mode)
            {
                ESP_LOGI(TAG, "🎯 Objetivos alcanzados, cambiando a modo mantenimiento");
                smart_config.maintenance_mode = true;
            }

            if (smart_config.maintenance_mode)
            {
                attempt_device_reconnection();
            }

            // 2. Lógica de Ciclo de Descubrimiento con Reintentos
            bool discovery_successful = false;
            int retry_count = 0;
            discovery_metrics.nuevos_dispositivos_en_ciclo = 0;

            while (retry_count < smart_config.max_retries && !discovery_successful)
            {
                if (retry_count > 0)
                {
                    ESP_LOGW(TAG, "🔄 Reintento %d/%d del ciclo de descubrimiento completo", retry_count, smart_config.max_retries);
                    if (!ble_smart_delay_or_stopped(2000))
                    {
                        break;
                    }
                }

                // === LÓGICA DE ESCANEO POR INTERVALOS ===
                const int num_bursts = 4;
                const int scan_duration_per_burst_ms = 4000;
                const int pause_between_bursts_ms = 1000;
                bool cycle_failed = false;

                for (int i = 0; i < num_bursts; i++)
                {
                    // VERIFICAR MEMORIA ANTES DE CADA RÁFAGA
                    free_heap = esp_get_free_heap_size();
                    if (free_heap < MIN_HEAP_BYTES * 1.5)
                    {
                        ESP_LOGW(TAG, "⚠️ Memoria baja antes de ráfaga %d (%zu bytes), saltando...", i + 1, free_heap);
                        break;
                    }

                    ESP_LOGI(TAG, "⚡️ Iniciando ráfaga de escaneo %d/%d...", i + 1, num_bursts);

                    // Llamamos a nuestra función de alto nivel con una duración corta
                    esp_err_t ret = ble_device_start_smart_discovery(scan_duration_per_burst_ms);

                    if (ret == ESP_OK)
                    {
                        if (xSemaphoreTake(scan_complete_semaphore, pdMS_TO_TICKS(scan_duration_per_burst_ms + 1000)) != pdTRUE)
                        {
                            ESP_LOGE(TAG, "💥 TIMEOUT: La ráfaga de escaneo %d no terminó a tiempo.", i + 1);
                            ble_device_stop_scan();
                            cycle_failed = true;
                            break;
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "❌ Error iniciando la ráfaga de escaneo %d.", i + 1);
                        cycle_failed = true;
                        break;
                    }

                    // No pausar en la última ráfaga
                    if (i < num_bursts - 1)
                    {
                        ESP_LOGI(TAG, "⏸️ Pausa de %d ms para ceder paso a WiFi/WebRTC...", pause_between_bursts_ms);

                        // PAUSA MÁS LARGA SI MEMORIA BAJA
                        int pause_time = pause_between_bursts_ms;
                        if (free_heap < MIN_HEAP_BYTES * 2)
                        {
                            pause_time *= 2;
                            ESP_LOGW(TAG, "🐌 Pausa extendida por memoria baja");
                        }

                        if (!ble_smart_delay_or_stopped(pause_time))
                        {
                            cycle_failed = true;
                            break;
                        }
                    }
                }
                // === FIN DE LÓGICA DE INTERVALOS ===

                if (!cycle_failed)
                {

                    // --- INICIO DEL NUEVO BLOQUE DE ACCIÓN ---
                    // 1. Revisar si el explorador encontró un candidato.
                    if (best_candidate.found)
                    {
                        ESP_LOGW(TAG, "🏆 Intentando perfilar al mejor candidato encontrado (RSSI: %d)", best_candidate.rssi);

                        // 1. Rellenar el expediente con los datos del candidato
                        if (xSemaphoreTake(devices_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            memcpy(g_pending_connection.addr, best_candidate.addr, 6);
                            g_pending_connection.addr_type = best_candidate.addr_type;
                            g_pending_connection.is_active = true;
                            xSemaphoreGive(devices_mutex);
                        }

                        // 2. Ahora, iniciar la operación de conexión
                        esp_err_t conn_result = ble_device_connect(best_candidate.addr, best_candidate.addr_type);
                        if (conn_result == ESP_OK)
                        {
                            INCR_ACTIVE_OPS();
                        }
                        else
                        {
                            // Si el inicio falla, limpiar el expediente inmediatamente
                            g_pending_connection.is_active = false;
                        }
                    }
                    // --- FIN DEL NUEVO BLOQUE DE ACCIÓN ---

                    ESP_LOGI(TAG, "✅ Escaneo por intervalos completado. Esperando que terminen las operaciones de perfilado...");

                    // TIMEOUT MÁS LARGO CON VERIFICACIONES INTELIGENTES
                    int wait_cycles = 0;
                    const int MAX_WAIT_CYCLES = 600; // 60 segundos en lugar de 30
                    int last_operations_count = atomic_load(&g_active_ble_operations);
                    int stale_count = 0; // Contador para operaciones que no avanzan

                    while (smart_discovery_enabled &&
                           !module_stopping &&
                           atomic_load(&g_active_ble_operations) > 0 &&
                           wait_cycles < MAX_WAIT_CYCLES)
                    {
                        if (!ble_smart_delay_or_stopped(100))
                        {
                            break;
                        }
                        wait_cycles++;

                        int current_ops = atomic_load(&g_active_ble_operations);

                        // Verificar progreso cada 5 segundos
                        if (wait_cycles % 50 == 0)
                        {
                            ESP_LOGI(TAG, "⏳ Esperando %d operaciones de perfilado (ciclo %d/%d)",
                                     current_ops, wait_cycles / 10, MAX_WAIT_CYCLES / 10);

                            // Si no hay progreso en 15 segundos, forzar limpieza
                            if (current_ops == last_operations_count)
                            {
                                stale_count++;
                                if (stale_count >= 3)
                                { // 15 segundos sin cambios
                                    ESP_LOGW(TAG, "⚠️ Operaciones bloqueadas detectadas, forzando limpieza...");
                                    cleanup_stale_connections();
                                    force_cleanup_stuck_operations();
                                    break;
                                }
                            }
                            else
                            {
                                stale_count = 0;
                                last_operations_count = current_ops;
                            }
                        }

                        // Early exit si memoria muy baja
                        if (wait_cycles % 10 == 0)
                        { // Verificar cada segundo
                            free_heap = esp_get_free_heap_size();
                            if (free_heap < MIN_HEAP_BYTES)
                            {
                                ESP_LOGW(TAG, "🛑 Memoria crítica durante espera (%zu bytes), abortando operaciones", free_heap);
                                force_cleanup_stuck_operations();
                                break;
                            }
                        }
                    }

                    int final_ops = atomic_load(&g_active_ble_operations);
                    if (final_ops == 0)
                    {
                        ESP_LOGI(TAG, "✅ Todas las operaciones de perfilado han terminado.");
                        discovery_successful = true;
                    }
                    else
                    {
                        ESP_LOGE(TAG, "💥 TIMEOUT: Quedan %d operaciones de perfilado sin terminar.", final_ops);
                        // Forzar limpieza final
                        force_cleanup_stuck_operations();
                    }
                }
                retry_count++;
            }

            // 3. Actualización de Estadísticas y Manejo de Fallos Consecutivos
            if (discovery_successful)
            {
                consecutive_failures = 0;
                smart_stats.successful_discoveries++;
            }
            else
            {
                consecutive_failures++;
                smart_stats.failed_discoveries++;
                ESP_LOGE(TAG, "💥 Falló el descubrimiento para el ciclo #%lu después de %d intentos.", smart_stats.total_cycles, smart_config.max_retries);
            }

            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES)
            {
                ESP_LOGW(TAG, "⚠️ Demasiados fallos consecutivos, pausa extendida de 2 minutos...");
                if (!ble_smart_delay_or_stopped(120000))
                {
                    break;
                }
                consecutive_failures = 0;
            }

            // 4. Recopilación y Log de Estadísticas del Ciclo
            // --- CAMBIO CLAVE: Usar el Heap para el array temporal ---
            ble_device_info_t *devices = heap_caps_malloc(sizeof(ble_device_info_t) * BLE_DEVICE_MAX_DEVICES, BLE_STAGING_BUFFER_CAPS);
            if (devices == NULL)
            {
                ESP_LOGE(TAG, "Fallo al asignar memoria PSRAM para estadísticas, saltando ciclo.");
                // Liberar el mutex si se tomó antes, aunque en este punto no debería estarlo
                vTaskDelay(pdMS_TO_TICKS(5000)); // Esperar antes de reintentar
                continue;                        // Saltar al siguiente ciclo del while
            }
            int count = ble_device_get_discovered_list(devices, BLE_DEVICE_MAX_DEVICES);
            int connected_count = 0, error_count = 0, known_count = 0;
            for (int i = 0; i < count; i++)
            {
                if (devices[i].state == BLE_DEVICE_STATE_CONNECTED)
                    connected_count++;
                if (devices[i].state == BLE_DEVICE_STATE_ERROR)
                    error_count++;
                if (devices[i].is_known)
                    known_count++;
            }
            ESP_LOGI(TAG, "📊 ESTADÍSTICAS - Total: %d | Conectados: %d | Errores: %d | Conocidos: %d",
                     count, connected_count, error_count, known_count);
            ESP_LOGI(TAG, "📈 TOTALES - Ciclos: %lu | Éxitos: %lu | Fallos: %lu",
                     smart_stats.total_cycles, smart_stats.successful_discoveries,
                     smart_stats.failed_discoveries);
            heap_caps_free(devices);

            // 5. Lógica de Parada Inteligente
            if (discovery_metrics.nuevos_dispositivos_en_ciclo == 0)
            {
                discovery_metrics.ciclos_vacios_consecutivos++;
            }
            else
            {
                discovery_metrics.ciclos_vacios_consecutivos = 0;
            }

            const uint32_t dynamic_idle_ms = (known_count == 0) ? IDLE_MS_FIRST_VISIT : IDLE_MS_SUBSEQUENT;
            const TickType_t now_ticks = xTaskGetTickCount();
            const uint32_t ms_since_last_new = (uint32_t)((now_ticks - discovery_metrics.tiempo_ultimo_nuevo) * portTICK_PERIOD_MS);
            free_heap = esp_get_free_heap_size();
            bool stop_now = false;

            if (discovery_metrics.ciclos_vacios_consecutivos >= MAX_EMPTY_CYCLES)
            {
                ESP_LOGI(TAG, "🛑 Parada inteligente: %" PRIu32 " ciclos sin novedades.", discovery_metrics.ciclos_vacios_consecutivos);
                stop_now = true;
            }
            else if (ms_since_last_new >= dynamic_idle_ms)
            {
                ESP_LOGI(TAG, "🛑 Parada inteligente: %" PRIu32 " ms sin nuevos dispositivos (umbral %" PRIu32 " ms).", ms_since_last_new, dynamic_idle_ms);
                stop_now = true;
            }
            else if (free_heap < MIN_HEAP_BYTES)
            {
                ESP_LOGW(TAG, "🛑 Parada por memoria baja: heap libre=%zu bytes (< %zu).", free_heap, MIN_HEAP_BYTES);
                stop_now = true;
            }

            if (stop_now)
            {
                smart_discovery_enabled = false;
                break;
            }

            // 6. Lógica de Espera entre Ciclos
            uint32_t wait_time_ms = smart_config.maintenance_mode ? smart_config.scan_interval_maintenance_ms : smart_config.scan_interval_normal_ms;
            ESP_LOGI(TAG, "%s: esperando %lu segundos...",
                     smart_config.maintenance_mode ? "💤 Modo mantenimiento" : "⏰ Modo normal",
                     wait_time_ms / 1000);

            uint32_t wait_chunks = wait_time_ms / 1000;
            for (uint32_t i = 0; i < wait_chunks && smart_discovery_enabled && !module_stopping; i++)
            {
                if (!ble_smart_delay_or_stopped(1000))
                {
                    break;
                }
            }
        }

    task_exit:
        if (smart_task_mode == BLE_SMART_TASK_MODE_DISCOVERY &&
            !module_stopping &&
            device_callbacks.on_discovery_stopped)
        {
            device_callbacks.on_discovery_stopped(discovery_metrics.nuevos_dispositivos_en_ciclo);
        }
        smart_discovery_running = false;
        smart_discovery_enabled = false;
        smart_task_mode = BLE_SMART_TASK_MODE_DISCOVERY;

        if (smart_task_idle_signal != NULL)
        {
            xSemaphoreGive(smart_task_idle_signal);
        }

        ESP_LOGW(TAG, "✅ Tarea de Agente de Descubrimiento ha completado sus ciclos.");
    }
    ESP_LOGI(TAG, "Tarea persistente de Agente de Descubrimiento saliendo.");
    smart_discovery_running = false;
    smart_discovery_enabled = false;
    smart_task_mode = BLE_SMART_TASK_MODE_DISCOVERY;
    smart_ble_task_handle = NULL;

    if (smart_task_idle_signal != NULL)
    {
        xSemaphoreGive(smart_task_idle_signal);
    }

    vTaskDelete(NULL);
}

/**
 * @brief Initialize the smart discovery system
 * This function resets statistics and sets default values.
 * It should be called before starting the smart discovery task.
 */
esp_err_t ble_device_smart_system_init(void)
{
    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "❌ Módulo BLE no está inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    // Resetear estadísticas
    memset(&smart_stats, 0, sizeof(smart_stats));
    smart_discovery_enabled = true;

    // --- CENTRALIZAR CONFIGURACIÓN INICIAL ---
    // Llamar a la función de configuración con los valores por defecto.
    ESP_LOGI(TAG, "Estableciendo configuración por defecto del descubrimiento inteligente...");
    esp_err_t config_err = ble_device_configure_smart_discovery(
        90000,  // scan_interval_normal_ms: 1 minuto
        600000, // scan_interval_maintenance_ms: 5 minutos
        3,      // target_devices
        false   // auto_test_commands (valor por defecto)
    );

    if (config_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al aplicar la configuración por defecto.");
        return config_err;
    }
    // El modo mantenimiento siempre empieza en false, independientemente de la config.
    smart_config.maintenance_mode = false;

    ESP_LOGI(TAG, "✅ Sistema de descubrimiento inteligente inicializado y configurado.");
    return ESP_OK;
}

/**
 * @brief Initializes the smart discovery task
 * This function checks if the BLE module is initialized and starts the smart discovery task.
 * It should be called to start the smart discovery process.
 * If the task is already running, it returns an error.
 * If the task is created successfully, it returns ESP_OK.
 * If the task creation fails, it returns an error code.
 */
esp_err_t ble_device_start_smart_task(void)
{
    if (!ble_central_diagnostic_enabled())
    {
        ESP_LOGW(TAG, "Smart task rejected; BLE Central diagnostic is disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (!module_initialized || module_stopping)
    {
        ESP_LOGE(TAG, "Modulo BLE no esta inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_ble_task_handle == NULL ||
        smart_task_start_signal == NULL ||
        smart_task_idle_signal == NULL)
    {
        ESP_LOGE(TAG, "Recursos persistentes de smart task no estan listos");
        return ESP_ERR_INVALID_STATE;
    }

    if (smart_discovery_running || smart_discovery_enabled)
    {
        ESP_LOGW(TAG, "La tarea BLE inteligente ya esta activa");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Activando tarea persistente de descubrimiento inteligente");

    esp_err_t ret = ble_device_smart_system_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error inicializando sistema inteligente: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(&discovery_metrics, 0, sizeof(discovery_metrics));
    discovery_metrics.tiempo_ultimo_nuevo = xTaskGetTickCount();

    xSemaphoreTake(smart_task_idle_signal, 0);
    xSemaphoreTake(smart_task_start_signal, 0);

    smart_task_mode = BLE_SMART_TASK_MODE_DISCOVERY;
    smart_discovery_enabled = true;
    smart_discovery_running = true;

    if (xSemaphoreGive(smart_task_start_signal) != pdTRUE)
    {
        smart_discovery_enabled = false;
        smart_discovery_running = false;
        ESP_LOGE(TAG, "No se pudo activar la tarea BLE inteligente persistente");
        return ESP_FAIL;
    }

    ble_log_memory_snapshot("smart_task:activated");
    return ESP_OK;
}

/**
 * @brief Stops the smart discovery task
 * This function stops the smart discovery task if it is running.
 * It waits for the task to finish and cleans up resources.
 */
esp_err_t ble_device_stop_smart_task(void)
{
    if (smart_ble_task_handle == NULL)
    {
        ESP_LOGW(TAG, "La tarea BLE inteligente persistente no existe");
        return ESP_OK;
    }

    if (!smart_discovery_running && !smart_discovery_enabled)
    {
        ESP_LOGW(TAG, "La tarea BLE inteligente ya esta en reposo");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deteniendo actividad de descubrimiento inteligente...");

    smart_discovery_enabled = false;

    if (scanning_active)
    {
        esp_err_t stop_err = ble_device_stop_scan();
        if (stop_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Scan stop during smart task stop failed: %s", esp_err_to_name(stop_err));
        }
    }

    if (smart_task_start_signal != NULL)
    {
        xSemaphoreGive(smart_task_start_signal);
    }

    if (smart_task_idle_signal != NULL)
    {
        if (xSemaphoreTake(smart_task_idle_signal, pdMS_TO_TICKS(BLE_SMART_TASK_STOP_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Timeout esperando reposo de smart task; conservando tarea persistente");
            smart_discovery_running = false;
            smart_discovery_enabled = false;
            return ESP_ERR_TIMEOUT;
        }
    }

    smart_discovery_running = false;
    smart_discovery_enabled = false;

    ESP_LOGI(TAG, "Actividad de descubrimiento inteligente detenida");
    return ESP_OK;
}
