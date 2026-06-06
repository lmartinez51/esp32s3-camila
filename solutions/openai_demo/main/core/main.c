/**
 * @file main.c
 * @brief Main application entry point for ESP32-S3-BOX3 AI Chatbot using OpenAI Realtime API.
 *
 * This application creates an AI chatbot that uses OpenAI's Realtime API through WebRTC.
 * It initializes the system components including UI, audio processing, WiFi connectivity,
 * and BLE-based WiFi provisioning as a fallback connection method.
 *
 * The system automatically starts WebRTC communication once WiFi is connected and
 * provides visual feedback through the LCD display.
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
#include <nvs_flash.h>
// Includes de ESP-IDF
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "freertos/idf_additions.h"

// Includes de WebRTC y Media
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"

// Includes del proyecto
#include "common.h"
#include "ble_config.h"
#include "ble_common.h"
#include "codec_init.h"
#include "ui.h"
#include "mute_handler.h"
#include "bsp/esp-bsp.h"
#include "nvs_setup.h"
#include "ble_device_callbacks.h" // Callbacks para control de dispositivos BLE
#include "ble_device_control.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "app_events.h"
#include "wifi_session_state.h"
#include "config_manager.h"
#include "webrtc.h"
#include "csi_handler.h"

EventGroupHandle_t app_startup_event_group;
static const char *TAG = "MAIN";
static SemaphoreHandle_t ble_sync_semaphore = NULL;
static QueueHandle_t s_orchestrator_event_queue = NULL;
#define ORCHESTRATOR_QUEUE_DEPTH 8
#define ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS 200
#define IDENTITY_VALIDATION_TIMEOUT_MS BLE_DEVICE_SCAN_TIMEOUT_MS
#define BLE_RELEASE_TIMEOUT_MS 5000
#define AUTO_SLEEP_TIMEOUT_MS (5 * 60 * 1000)
#define AUTO_SLEEP_POLL_MS 1000

static bool s_audio_runtime_ready = false;
static bool s_arrival_context_sent = false;
static TaskHandle_t s_ble_release_task_handle = NULL;

typedef enum
{
    STATE_WAIT_WIFI = 0,
    STATE_SLEEP,
    STATE_VALIDATING_IDENTITY,
    STATE_RELEASING_BLE,
    STATE_IGNITING,
    STATE_ACTIVE,
    STATE_AUTO_SLEEPING,
} orchestrator_state_t;
#define ENABLE_ONE_TIME_PROVISIONING 0 // Pon esto en 0 para deshabilitarlo después del primer arranque

static uint32_t orchestrator_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void orchestrator_log_heap_snapshot(const char *stage)
{
    const char *label = stage ? stage : "unknown";

    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGW(TAG,
             "[HEAP] %s | INTERNAL free=%zu min=%zu largest=%zu | PSRAM free=%zu min=%zu largest=%zu",
             label,
             internal_free,
             internal_min,
             internal_largest,
             psram_free,
             psram_min,
             psram_largest);
}

static esp_err_t orchestrator_ensure_audio_runtime_ready(void)
{
    if (s_audio_runtime_ready)
    {
        return ESP_OK;
    }

    orchestrator_log_heap_snapshot("audio_runtime:before");
    init_board();
    int media_ret = media_sys_buildup();
    if (media_ret != 0)
    {
        ESP_LOGE(TAG, "Audio/media runtime init failed: %d", media_ret);
        orchestrator_log_heap_snapshot("audio_runtime:media_failed");
        return ESP_FAIL;
    }

    orchestrator_log_heap_snapshot("audio_runtime:media_ready");
    if (!media_sys_is_ready())
    {
        ESP_LOGE(TAG, "Audio/media runtime reported success but is not ready");
        orchestrator_log_heap_snapshot("audio_runtime:not_ready");
        return ESP_FAIL;
    }

    s_audio_runtime_ready = true;
    return ESP_OK;
}

static bool orchestrator_active_idle_expired(void)
{
    uint32_t last_activity_ms = webrtc_get_last_activity_ms();
    if (last_activity_ms == 0 || webrtc_realtime_is_busy())
    {
        return false;
    }

    return (uint32_t)(orchestrator_now_ms() - last_activity_ms) >= AUTO_SLEEP_TIMEOUT_MS;
}

/**
 * @brief Macro to execute tasks asynchronously in separate threads.
 *
 * This macro creates a temporary function that executes the given body
 * asynchronously and automatically destroys the thread when finished.
 * Essential for non-blocking WebRTC operations.
 *
 * @param name Thread identifier name (used for function naming)
 * @param body Code block to execute asynchronously
 */
#define RUN_ASYNC(name, body)           \
    void run_async##name(void *arg)     \
    {                                   \
        body;                           \
        media_lib_thread_destroy(NULL); \
    }                                   \
    media_lib_thread_create_from_scheduler(NULL, #name, run_async##name, NULL);

/**
 * @brief Thread scheduler configuration for optimal performance.
 *
 * Configures thread parameters (stack size, priority, CPU core affinity)
 * based on thread name to optimize system performance for real-time audio/video processing.
 * Different threads are assigned to different CPU cores for load balancing.
 *
 * @param thread_name Name of the thread to configure
 * @param thread_cfg Thread configuration structure to populate
 */
static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *thread_cfg)
{
    // WebRTC peer connection task - Core 1, alta prioridad
    if (strcmp(thread_name, "pc_task") == 0)
    {
        thread_cfg->stack_size = 24 * 1024; // Stack grande para procesamiento WebRTC
        thread_cfg->priority = 18;          // Prioridad alta
        thread_cfg->core_id = 1;            // Core 1 para WebRTC
    }
    // WebRTC data sending task - Core 1, prioridad media-alta
    if (strcmp(thread_name, "pc_send") == 0)
    {
        thread_cfg->stack_size = 3 * 1024;
        thread_cfg->priority = 15;
        thread_cfg->core_id = 1;
    }
    // Audio decoder task - Core 1, prioridad media
    if (strcmp(thread_name, "Adec") == 0)
    {
        thread_cfg->stack_size = 24 * 1024; // Stack grande para decodificación de audio
        thread_cfg->priority = 10;
        thread_cfg->core_id = 1;
    }
    // Video encoder task - configuración específica para S3
    if (strcmp(thread_name, "venc") == 0)
    {
#if CONFIG_IDF_TARGET_ESP32S3
        thread_cfg->stack_size = 20 * 1024; // Stack optimizado para S3
#endif
        thread_cfg->priority = 10;
    }
#ifdef WEBRTC_SUPPORT_OPUS
    // Audio encoder task - solo si OPUS está habilitado
    if (strcmp(thread_name, "aenc") == 0)
    {
        thread_cfg->stack_size = 40 * 1024; // Stack grande para codificación OPUS
        thread_cfg->priority = 10;
    }
    // Audio source reading task - Core 0, prioridad alta
    if (strcmp(thread_name, "SrcRead") == 0)
    {
        thread_cfg->stack_size = 40 * 1024;
        thread_cfg->priority = 16; // Prioridad alta para captura de audio
        thread_cfg->core_id = 0;   // Core 0 para balanceo de carga
    }
    // Audio buffer input task - Core 0, prioridad media
    if (strcmp(thread_name, "buffer_in") == 0)
    {
        thread_cfg->stack_size = 6 * 1024;
        thread_cfg->priority = 10;
        thread_cfg->core_id = 0;
    }
#endif
}

/**
 * @brief Helper function to start BLE provisioning mode with logging.
 *
 * This function ensures BLE is ready, acquires ownership for provisioning,
 * and starts advertising for WiFi provisioning. It logs each step and handles errors gracefully.
 *
 * @param reason Optional string describing the reason for entering provisioning mode (for logging)
 */
static void start_ble_provisioning_mode(const char *reason)
{
    ESP_LOGI(TAG, "Activando BLE provisioning: %s", reason ? reason : "");

    esp_err_t err = ble_common_ensure_ready(ble_sync_semaphore, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo preparar BLE provisioning: %s", esp_err_to_name(err));
        return;
    }

    err = ble_common_acquire(BLE_COMMON_ROLE_PROVISIONING);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo adquirir BLE provisioning ownership: %s", esp_err_to_name(err));
        return;
    }

    ble_wifi_start_advertising();
}

/**
 * @brief Queue-driven orchestrator helpers for the on-demand state machine.
 * This function converts orchestrator states to human-readable strings for logging purposes, improving log clarity and debugging.
 */
static const char *orchestrator_state_name(orchestrator_state_t state)
{
    switch (state)
    {
    case STATE_WAIT_WIFI:
        return "STATE_WAIT_WIFI";
    case STATE_SLEEP:
        return "STATE_SLEEP";
    case STATE_VALIDATING_IDENTITY:
        return "STATE_VALIDATING_IDENTITY";
    case STATE_RELEASING_BLE:
        return "STATE_RELEASING_BLE";
    case STATE_IGNITING:
        return "STATE_IGNITING";
    case STATE_ACTIVE:
        return "STATE_ACTIVE";
    case STATE_AUTO_SLEEPING:
        return "STATE_AUTO_SLEEPING";
    default:
        return "STATE_UNKNOWN";
    }
}

/**
 * @brief Helper to convert orchestrator events to string for logging.
 *
 * This function maps orchestrator_event_t values to human-readable strings for improved log clarity.
 *
 * @param event The orchestrator event to convert
 * @return A string representation of the event
 */
static const char *orchestrator_event_name(orchestrator_event_t event)
{
    switch (event)
    {
    case ORCH_EVENT_WIFI_CONNECTED:
        return "ORCH_EVENT_WIFI_CONNECTED";
    case ORCH_EVENT_WIFI_DISCONNECTED:
        return "ORCH_EVENT_WIFI_DISCONNECTED";
    case ORCH_EVENT_MOTION_DETECTED:
        return "ORCH_EVENT_MOTION_DETECTED";
    case ORCH_EVENT_IDENTITY_PRESENT:
        return "ORCH_EVENT_IDENTITY_PRESENT";
    case ORCH_EVENT_IDENTITY_REJECTED:
        return "ORCH_EVENT_IDENTITY_REJECTED";
    case ORCH_EVENT_BLE_RELEASE_COMPLETE:
        return "ORCH_EVENT_BLE_RELEASE_COMPLETE";
    case ORCH_EVENT_BLE_RELEASE_FAILED:
        return "ORCH_EVENT_BLE_RELEASE_FAILED";
    case ORCH_EVENT_WEBRTC_CONNECTED:
        return "ORCH_EVENT_WEBRTC_CONNECTED";
    case ORCH_EVENT_WEBRTC_DISCONNECTED:
        return "ORCH_EVENT_WEBRTC_DISCONNECTED";
    case ORCH_EVENT_WEBRTC_API_ERROR:
        return "ORCH_EVENT_WEBRTC_API_ERROR";
    case ORCH_EVENT_AUTO_SLEEP_TIMEOUT:
        return "ORCH_EVENT_AUTO_SLEEP_TIMEOUT";
    default:
        return "ORCH_EVENT_UNKNOWN";
    }
}

/**
 * @brief Post an event to the orchestrator queue with logging.
 *
 * This function sends events to the orchestrator's event queue and logs the action.
 * If the queue is not ready or full, it logs a warning and drops the event.
 *
 * @param event The orchestrator event to post
 */
void orchestrator_post_event(orchestrator_event_t event)
{
    if (s_orchestrator_event_queue == NULL)
    {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping event=%d", event);
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &event,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(event));
    }
}

static void orchestrator_start_identity_validation(void)
{
    ESP_LOGI(TAG, "Preparing bounded BLE identity validation (%d ms)", IDENTITY_VALIDATION_TIMEOUT_MS);
    orchestrator_log_heap_snapshot("identity_validation:start");

    esp_err_t err = ble_common_ensure_ready(ble_sync_semaphore, false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE host not ready for identity validation: %s", esp_err_to_name(err));
        orchestrator_log_heap_snapshot("identity_validation:ble_not_ready");
        orchestrator_post_event(ORCH_EVENT_IDENTITY_REJECTED);
        return;
    }

    err = ble_device_control_start(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE central control not ready for identity validation: %s", esp_err_to_name(err));
        orchestrator_log_heap_snapshot("identity_validation:central_failed");
        orchestrator_post_event(ORCH_EVENT_IDENTITY_REJECTED);
        return;
    }

    err = ble_device_start_identity_validation(IDENTITY_VALIDATION_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE identity validation could not be scheduled: %s", esp_err_to_name(err));
        ble_device_control_stop();
        orchestrator_log_heap_snapshot("identity_validation:schedule_failed");
        orchestrator_post_event(ORCH_EVENT_IDENTITY_REJECTED);
        return;
    }

    orchestrator_log_heap_snapshot("identity_validation:scheduled");
}

static void orchestrator_ble_release_task(void *param)
{
    (void)param;

    orchestrator_log_heap_snapshot("ble_release:before");
    esp_err_t err = ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
    orchestrator_log_heap_snapshot((err == ESP_OK) ? "ble_release:complete" : "ble_release:failed");

    orchestrator_post_event((err == ESP_OK)
                                ? ORCH_EVENT_BLE_RELEASE_COMPLETE
                                : ORCH_EVENT_BLE_RELEASE_FAILED);

    s_ble_release_task_handle = NULL;
    vTaskDelete(NULL);
}

static void orchestrator_start_ble_release(void)
{
    if (s_ble_release_task_handle != NULL)
    {
        ESP_LOGW(TAG, "BLE release already running");
        return;
    }

    BaseType_t rc = xTaskCreate(orchestrator_ble_release_task,
                                "ble_release",
                                4096,
                                NULL,
                                6,
                                &s_ble_release_task_handle);
    if (rc != pdPASS)
    {
        s_ble_release_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BLE release task");
        orchestrator_log_heap_snapshot("ble_release:create_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_RELEASE_FAILED);
    }
}

/**
 * @brief Enter a new state in the orchestrator state machine.
 *
 * This function transitions the orchestrator to a new state and performs the necessary actions for the state change.
 *
 * @param state A pointer to the current orchestrator state
 * @param next_state The state to transition to
 */
static void orchestrator_enter_state(orchestrator_state_t *state, orchestrator_state_t next_state)
{
    if (state == NULL)
    {
        return;
    }

    if (*state == next_state)
    {
        ESP_LOGI(TAG, "Orchestrator remains in %s", orchestrator_state_name(next_state));
        return;
    }

    ESP_LOGI(TAG, "Orchestrator transition: %s -> %s",
             orchestrator_state_name(*state),
             orchestrator_state_name(next_state));
    orchestrator_log_heap_snapshot("state_transition:before");

    *state = next_state;

    switch (next_state)
    {
    case STATE_WAIT_WIFI:
        ESP_LOGI(TAG, "STATE_WAIT_WIFI: stopping CSI, WebRTC, and BLE smart/control tasks.");
        csi_handler_stop();
        stop_webrtc();
        ble_device_stop_smart_task();
        ble_device_control_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WIFI_CONNECTED_BIT | WEBRTC_CONNECTED_BIT |
                                 WEBRTC_DISCONNECTED_BIT | WEBRTC_API_ERROR_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        display_disconnected_message();
        break;

    case STATE_SLEEP:
    {
        ESP_LOGI(TAG, "STATE_SLEEP: WiFi is up; WebRTC remains off. Starting CSI motion sensing.");
        stop_webrtc();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT | WIFI_DISCONNECTED_BIT);
        esp_err_t csi_err = csi_handler_start();
        if (csi_err != ESP_OK)
        {
            ESP_LOGE(TAG, "STATE_SLEEP: failed to start CSI: %s", esp_err_to_name(csi_err));
        }
        break;
    }

    case STATE_VALIDATING_IDENTITY:
        ESP_LOGI(TAG, "STATE_VALIDATING_IDENTITY: motion detected; pausing CSI and starting BLE validation.");
        csi_handler_stop();
        orchestrator_start_identity_validation();

        // --- INICIO DE SIMULACIÓN TEMPORAL ---
        // ESP_LOGW(TAG, "SIMULACIÓN: Forzando identidad validada para probar WebRTC...");
        // orchestrator_post_event(ORCH_EVENT_IDENTITY_PRESENT);
        // --- FIN DE SIMULACIÓN TEMPORAL ---
        break;

    case STATE_RELEASING_BLE:
        ESP_LOGI(TAG, "STATE_RELEASING_BLE: releasing NimBLE before audio ignition.");
        csi_handler_stop();
        orchestrator_start_ble_release();
        break;

    case STATE_IGNITING:
    {
        ESP_LOGI(TAG, "STATE_IGNITING: initializing audio runtime and starting WebRTC.");
        csi_handler_stop();
        s_arrival_context_sent = false;

        orchestrator_log_heap_snapshot("igniting:before_audio");
        esp_err_t audio_err = orchestrator_ensure_audio_runtime_ready();
        if (audio_err != ESP_OK || !media_sys_is_ready())
        {
            ESP_LOGE(TAG, "STATE_IGNITING: audio runtime not ready; aborting WebRTC start");
            orchestrator_log_heap_snapshot("igniting:audio_failed");
            orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
            break;
        }
        orchestrator_log_heap_snapshot("igniting:audio_ready");

        int ret = start_webrtc();
        if (ret != 0)
        {
            EventBits_t bits = xEventGroupGetBits(app_startup_event_group);
            orchestrator_post_event((bits & WEBRTC_API_ERROR_BIT)
                                        ? ORCH_EVENT_WEBRTC_API_ERROR
                                        : ORCH_EVENT_WEBRTC_DISCONNECTED);
        }
        break;
    }

    case STATE_ACTIVE:
        ESP_LOGI(TAG, "STATE_ACTIVE: WebRTC active; injecting arrival context.");
        csi_handler_stop();
        if (!s_arrival_context_sent)
        {
            if (webrtc_inject_arrival_context() == 0)
            {
                s_arrival_context_sent = true;
            }
            else
            {
                ESP_LOGW(TAG, "Arrival context injection failed; will not retry in this active session");
                s_arrival_context_sent = true;
            }
        }
        break;

    case STATE_AUTO_SLEEPING:
        ESP_LOGI(TAG, "STATE_AUTO_SLEEPING: stopping WebRTC; audio teardown deferred.");
        s_arrival_context_sent = false;
        stop_webrtc();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT);
        break;
    }

    orchestrator_log_heap_snapshot("state_transition:after");
}

/**
 * @brief Helper to log ignored events in the orchestrator state machine.
 *
 * This function logs when an event is received that is not relevant for the current state,
 * improving visibility into the state machine's behavior and aiding debugging.
 *
 * @param state The current orchestrator state
 * @param event The event that was ignored
 */
static void orchestrator_ignore_event(orchestrator_state_t state, orchestrator_event_t event)
{
    ESP_LOGI(TAG, "Orchestrator ignoring %s in %s",
             orchestrator_event_name(event),
             orchestrator_state_name(state));
}

/**
 * @brief Orchestrator task that manages the state machine for WiFi, motion detection, identity validation, and WebRTC session management.
 *
 * This task runs an infinite loop that waits for events from the orchestrator event queue and transitions between states based on the received events.
 * It handles the logic for starting/stopping CSI motion sensing, managing WebRTC sessions, and controlling BLE-based identity validation (placeholder).
 *
 * @param param Unused parameter for task creation
 */
static void app_startup_orchestrator_task(void *param)
{
    (void)param;
    orchestrator_state_t state = STATE_WAIT_WIFI;
    orchestrator_event_t event;

    ESP_LOGI(TAG, "Orchestrator state machine started in %s", orchestrator_state_name(state));

    while (1)
    {
        TickType_t wait_ticks = (state == STATE_ACTIVE)
                                    ? pdMS_TO_TICKS(AUTO_SLEEP_POLL_MS)
                                    : portMAX_DELAY;

        if (xQueueReceive(s_orchestrator_event_queue, &event, wait_ticks) != pdTRUE)
        {
            if (state == STATE_ACTIVE && orchestrator_active_idle_expired())
            {
                event = ORCH_EVENT_AUTO_SLEEP_TIMEOUT;
            }
            else
            {
                continue;
            }
        }

        ESP_LOGI(TAG, "Orchestrator event: %s while in %s",
                 orchestrator_event_name(event),
                 orchestrator_state_name(state));

        switch (state)
        {
        case STATE_WAIT_WIFI:
            if (event == ORCH_EVENT_WIFI_CONNECTED)
            {
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else if (event != ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_SLEEP:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_MOTION_DETECTED)
            {
                orchestrator_enter_state(&state, STATE_VALIDATING_IDENTITY);
            }
            else if (event != ORCH_EVENT_WIFI_CONNECTED)
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_VALIDATING_IDENTITY:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_IDENTITY_PRESENT)
            {
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            }
            else if (event == ORCH_EVENT_IDENTITY_REJECTED)
            {
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_RELEASING_BLE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_BLE_RELEASE_COMPLETE)
            {
                orchestrator_enter_state(&state, STATE_IGNITING);
            }
            else if (event == ORCH_EVENT_BLE_RELEASE_FAILED)
            {
                orchestrator_log_heap_snapshot("ble_release:failed_to_sleep");
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_IGNITING:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_WEBRTC_CONNECTED)
            {
                orchestrator_enter_state(&state, STATE_ACTIVE);
            }
            else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                     event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_ACTIVE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_AUTO_SLEEP_TIMEOUT)
            {
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                     event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_AUTO_SLEEPING:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else
            {
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            break;
        }
    }
}

/**
 * @brief Network event handler for WiFi connection state changes.
 *
 * Handles network connectivity events by updating compatibility bits and
 * notifying the orchestrator state machine. WebRTC is not started here.
 *
 * @param connected True if network is connected, false if disconnected
 * @return 0 on successful event handling, non-zero on error
 */
// En main.c
static int network_event_handler(bool connected)
{
    if (connected)
    {
        ESP_LOGI(TAG, "Evento de Red: WiFi Conectado. Señalando WIFI_CONNECTED_BIT.");
        if (ble_common_get_owner() == BLE_COMMON_ROLE_PROVISIONING)
        {
            ESP_LOGI(TAG, "Wi-Fi conectado, desactivando BLE provisioning");
            ble_wifi_provisioning_deinit();
            ble_common_release(BLE_COMMON_ROLE_PROVISIONING);
        }
        // El orquestador es el único dueño del arranque/reintento de WebRTC.
        xEventGroupClearBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_CONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WIFI_CONNECTED);
    }
    else
    {
        ESP_LOGI(TAG, "Evento de Red: WiFi Desconectado.");
        // Limpiar las banderas si nos desconectamos
        xEventGroupClearBits(app_startup_event_group, WIFI_CONNECTED_BIT | WEBRTC_CONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WIFI_DISCONNECTED);
    }
    return 0;
}

/**
 * @brief Diagnostic task for Phase 0 Baseline.
 * Monitors Internal SRAM and PSRAM to detect memory leaks or fragmentation
 * during active WebRTC sessions.
 */
static void sys_telemetry_task(void *param)
{
    (void)param;

    while (1)
    {
        orchestrator_log_heap_snapshot("telemetry:periodic");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/**
 * @brief Main application entry point.
 *
 * Initializes all system components in the following order:
 * 1. UI and display system
 * 2. I2C bus and media libraries
 * 3. Board-specific initialization
 * 4. NVS (Non-Volatile Storage)
 * 5. Mute handler for audio control
 * 6. BLE WiFi provisioning system (optional if WiFi fails)
 * 7. WiFi connectivity
 * 8. BLE stack initialization
 *
 * After initialization, enters the main loop that periodically queries
 * WebRTC status. If WiFi connection fails, it displays credentials screen
 * for BLE-based WiFi provisioning.
 */
void app_main(void)
{
    // Configura el nivel de log para este módulo
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // 1) Inicializa la interfaz de usuario (pantalla LCD)
    esp_err_t err = ui_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falló la inicialización de la UI: %s", esp_err_to_name(err));
        return; // Error crítico - no podemos continuar sin UI
    }

    // 2) Inicializa componentes básicos del sistema
    bsp_i2c_init();                                     // Bus I2C para periféricos
    media_lib_add_default_adapter();                    // Adaptador de medios por defecto
    media_lib_thread_set_schedule_cb(thread_scheduler); // Configura scheduler de hilos

    // 3) Inicializa almacenamiento y manejadores
    init_nvs();                 // Non-Volatile Storage para configuraciones
    mute_handler_init();        // Manejador de silenciado de audio
    config_manager_init();      // Gestor de configuración
    webrtc_init_action_queue(); // Inicializa la cola/tarea de acciones WebRTC

    bool boot_to_provisioning = nvs_read_and_clear_boot_to_provisioning_flag();

#if ENABLE_ONE_TIME_PROVISIONING
    // --- AÑADE ESTE BLOQUE ---
    const char *current_ssid = "example-ssid"; // O tu SSID temporal de pruebas
    // 1. Guardar la "enciclopedia" de perfiles primero
    // nvs_provision_known_profiles();
    // 2. Guardar el dispositivo de prueba específico
    // nvs_provision_hue_test_device(current_ssid);
    // 3. Listar el contenido de NVS para verificar
    debug_nvs_contents(current_ssid);
    // 4. Limpiar entradas inválidas (si las hay)
    // clean_invalid_ble_entries_from_nvs();
    // 5. Listar todos los dispositivos guardados en NVS
    list_all_ble_devices_from_nvs();
    // 6. Listar caracteristicas
    list_all_characteristics_from_nvs();
    // 7. Borrar API Keys (si quieres probar desde cero)
    esp_err_t error = nvs_delete_api_key();
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "API Key eliminada de NVS");
    }
    else
    {
        ESP_LOGE(TAG, "Error al eliminar: %s", esp_err_to_name(error));
    }
    vTaskDelay(pdMS_TO_TICKS(500)); // Espera 0.5 segundos para que se vea el log
    // 8. Listar la API Key guardada en NVS (para depuración)
    list_api_keys_from_nvs();
    // -------------------------
#endif

    // 4) Crear primitivas de sincronización
    // Crear el grupo de eventos para sincronización de arranque
    app_startup_event_group = xEventGroupCreate();
    // Crear el semáforo que usaremos para esperar la sincronización
    ble_sync_semaphore = xSemaphoreCreateBinary();
    s_orchestrator_event_queue = xQueueCreate(ORCHESTRATOR_QUEUE_DEPTH, sizeof(orchestrator_event_t));
    if (app_startup_event_group == NULL || ble_sync_semaphore == NULL || s_orchestrator_event_queue == NULL)
    {
        ESP_LOGE(TAG, "No se pudieron crear primitivas de sincronizacion de arranque");
        return;
    }

    // 5) BLE se inicializa bajo demanda para no competir con el primer intento WiFi.
    ESP_LOGI(TAG, "BLE se inicializara bajo demanda.");
    ESP_LOGI(TAG, "BLE Central permanece deshabilitado por defecto.");

    // 6) Iniciar tarea orquestadora de arranque
    xTaskCreate(app_startup_orchestrator_task, "startup_orch", 4096, NULL, 5, NULL);

    bool wifi_connected = false;
    if (boot_to_provisioning)
    {
        // Si arrancamos forzados a provisioning, mostramos esa pantalla de inmediato
        ESP_LOGW(TAG, "Mostrando pantalla de Modo Configuración en arranque.");
        display_config_mode_message();
    }
    else
    {
        // Flujo normal: intentar conectar a WiFi.
        ESP_LOGI(TAG, "Inicializando WiFi. WebRTC queda bajo demanda.");
        display_startup_screen();
        ESP_ERROR_CHECK(network_wifi_init(network_event_handler));
        wifi_connected = network_wifi_connect_main(WIFI_SSID, WIFI_PASSWORD);
    }

    if (wifi_connected)
    {
        ESP_LOGI(TAG, "WiFi conectado. El Orchestrator se encargara de WebRTC.");
    }
    else
    {
        // Solo mostramos 'Enter WiFi' si NO venimos de un reinicio forzado
        if (!boot_to_provisioning)
        {
            ESP_LOGI(TAG, "WiFi no conectado (fallo normal). Mostrando pantalla de credenciales WiFi.");
            display_wifi_creds();
        }
        else
        {
            // Si boot_to_provisioning es true, la pantalla de "Config Mode" ya se mostró antes.
            // No hacemos nada aquí para no sobrescribirla.
            ESP_LOGW(TAG, "WiFi no conectado (forzado por bandera). Pantalla 'Config Mode' ya visible.");
        }
        // Este flujo SÍ funciona y se queda igual. Si no hay WiFi, entramos a provisioning de inmediato.
        start_ble_provisioning_mode(boot_to_provisioning ? "Arranque forzado a provisioning." : "WiFi agoto reintentos.");
    }

    // Spawn Baseline Telemetry Task (Pinned to Core 1, Low Priority)
    xTaskCreatePinnedToCoreWithCaps(sys_telemetry_task, "telemetry_task", 3072, NULL,
                                    tskIDLE_PRIORITY + 1, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // 7) Loop principal - monitorea el estado de WebRTC continuamente
    while (1)
    {
        media_lib_thread_sleep(2000); // Espera 2 segundos entre consultas
        query_webrtc();               // Consulta estado actual de WebRTC
    }
}
