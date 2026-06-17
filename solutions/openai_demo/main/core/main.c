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
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <nvs_flash.h>
// Includes de ESP-IDF
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"

// Includes de WebRTC y Media
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"

// Includes del proyecto
#include "hardware/radar.h"
#include "common.h"
#include "ble_config.h"
#include "ble_common.h"
#include "codec_init.h"
#include "ui.h"
#include "simi.h"
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
#include "ei_inference.h"
#include "alert_dispatcher.h"
#include "aht30.h"

EventGroupHandle_t app_startup_event_group;
static const char *TAG = "MAIN";
static SemaphoreHandle_t ble_sync_semaphore = NULL;

#define SENSOR_I2C_SDA_PIN 41
#define SENSOR_I2C_SCL_PIN 40

static i2c_master_bus_handle_t sensor_board_i2c_init(void)
{
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1, // Auto select port
        .scl_io_num = SENSOR_I2C_SCL_PIN,
        .sda_io_num = SENSOR_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize external I2C bus: %s", esp_err_to_name(err));
        return NULL;
    }
    return bus_handle;
}

bool g_hardware_radar_present = false;
static bool s_aht30_present = false;
static aht30_dev_handle_t s_aht30_handle = NULL;
static uint32_t s_last_aht30_poll_ms = 0;

static void aht30_init_once(void)
{
    ESP_LOGI(TAG, "Initializing External I2C Bus for Sensor Dock (10kHz)...");
    i2c_master_bus_handle_t bus_handle = sensor_board_i2c_init();
    if (!bus_handle) {
        ESP_LOGW(TAG, "Failed to get External I2C handle. Sensor disabled.");
        return;
    }

    // --- NON-DESTRUCTIVE I2C PING TO SENSOR DOCK (0x28) ---
    esp_err_t ping_err = i2c_master_probe(bus_handle, 0x28, -1);
    if (ping_err == ESP_OK) {
        g_hardware_radar_present = true;
        ESP_LOGI(TAG, "Sensor Dock detected at 0x28. Hardware Radar Presence ENABLED.");
    } else {
        ESP_LOGI(TAG, "No device at 0x28. Software CSI Presence ENABLED.");
    }
    // ------------------------------------------------------

    if (aht30_init(bus_handle, &s_aht30_handle) == ESP_OK) {
        s_aht30_present = true;
        ESP_LOGI(TAG, "AHT30 initialized successfully. Tolerant Architecture enabled.");
    } else {
        ESP_LOGW(TAG, "AHT30 not found on bus. Hardware optionality triggered (Disabled).");
    }
}

static void poll_and_draw_temperature(void)
{
    if (!s_aht30_present || !s_aht30_handle) {
        return;
    }

    float temp = 0.0f;
    esp_err_t err = aht30_read_temp_humid(s_aht30_handle, &temp, NULL);
    
    char temp_str[16];
    if (err == ESP_OK) {
        snprintf(temp_str, sizeof(temp_str), "%.1f C", temp);
    } else {
        snprintf(temp_str, sizeof(temp_str), "-- C");
        ESP_LOGW(TAG, "AHT30 read failed mid-session. Continuing gracefully.");
    }

    // Pass the text to the Dr. Simi UI renderer for safe double-buffered compositing
    ui_simi_set_temperature_text(temp_str);
}
QueueHandle_t s_orchestrator_event_queue = NULL;
static bool s_is_muted = false;
#define ORCHESTRATOR_QUEUE_DEPTH 8
#define ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS 200
#define IDENTITY_VALIDATION_TIMEOUT_MS BLE_DEVICE_SCAN_TIMEOUT_MS
#define BLE_RELEASE_TIMEOUT_MS 5000
#define AUTO_SLEEP_TIMEOUT_MS (5 * 60 * 1000)
#define AUTO_SLEEP_POLL_MS 1000
#define VIGILANTE_REINFORCEMENT_DELAY_MS 30000
#define VIGILANTE_VACATED_CONFIRM_MS 10000
#define VIGILANTE_ACTIVE_TIMEOUT_MS (5 * 60 * 1000)
#define VIGILANTE_STATUS_STALE_MS 5000
#define SLEEP_CSI_COOLDOWN_MS 4000
#define SLEEP_WIFI_READY_DISPLAY_MS 900
#define WEBRTC_STOP_TIMEOUT_MS 8000
#define WEBRTC_STOP_TASK_STACK_SIZE (6 * 1024)
#define WEBRTC_STOP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define ORCHESTRATOR_EXTERNAL_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static bool s_arrival_context_sent = false;
static TaskHandle_t s_ble_prepare_task_handle = NULL;
static TaskHandle_t s_ble_release_task_handle = NULL;
static TaskHandle_t s_webrtc_stop_task_handle = NULL;
static TaskHandle_t s_alert_dispatch_task_handle = NULL;
static TaskHandle_t s_vigilante_reinforcement_task_handle = NULL;
static uint32_t s_alert_timestamp_ms = 0;
static float s_alert_corr_drop = 0.0f;
static bool s_alert_dispatch_pending = false;
static webrtc_session_mode_t s_pending_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
static webrtc_session_mode_t s_ignition_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
static webrtc_session_mode_t s_active_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
static bool s_ble_release_to_sleep = false;
static uint32_t s_vigilante_active_started_ms = 0;
static uint32_t s_vigilante_resting_since_ms = 0;
static bool s_vigilante_reinforcement_sent = false;
static volatile uint32_t s_sleep_csi_generation = 0;
static uint32_t s_sleep_motion_allowed_ms = 0;
static uint32_t s_webrtc_stop_started_ms = 0;

typedef enum
{
    STATE_WAIT_WIFI = 0,
    STATE_SLEEP,
    STATE_PREPARING_BLE,
    STATE_VALIDATING_IDENTITY,
    STATE_RELEASING_BLE,
    STATE_DISPATCHING_ALERT,
    STATE_IGNITING,
    STATE_ACTIVE,
    STATE_AUTO_SLEEPING,
    STATE_STOPPING_WEBRTC,
} orchestrator_state_t;
#define ENABLE_ONE_TIME_PROVISIONING 0 // Pon esto en 0 para deshabilitarlo después del primer arranque

static void orchestrator_show_phase(const char *reason,
                                    const char *title,
                                    const char *subtitle,
                                    uint16_t color);

static uint32_t orchestrator_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void app_heap_alloc_failed_hook(size_t requested_size,
                                       uint32_t caps,
                                       const char *function_name)
{
    ESP_EARLY_LOGE(TAG,
                   "[HEAP] alloc_failed | size=%u caps=0x%08" PRIx32 " fn=%s | INTERNAL free=%u largest=%u | DMA free=%u largest=%u | PSRAM free=%u largest=%u",
                   (unsigned)requested_size,
                   caps,
                   function_name ? function_name : "?",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static BaseType_t orchestrator_create_external_stack_task(TaskFunction_t task_fn,
                                                          const char *name,
                                                          const uint32_t stack_depth,
                                                          void *param,
                                                          UBaseType_t priority,
                                                          TaskHandle_t *task_handle)
{
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    return xTaskCreatePinnedToCoreWithCaps(task_fn,
                                           name,
                                           stack_depth,
                                           param,
                                           priority,
                                           task_handle,
                                           tskNO_AFFINITY,
                                           ORCHESTRATOR_EXTERNAL_STACK_CAPS);
#else
    return xTaskCreate(task_fn, name, stack_depth, param, priority, task_handle);
#endif
}

static void reset_alert_context(void)
{
    s_alert_timestamp_ms = 0;
    s_alert_corr_drop = 0.0f;
    s_alert_dispatch_pending = false;
    s_alert_dispatch_task_handle = NULL;
    s_pending_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
}

static void reset_vigilante_runtime_context(void)
{
    s_active_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
    s_ignition_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
    s_vigilante_active_started_ms = 0;
    s_vigilante_resting_since_ms = 0;
    s_vigilante_reinforcement_sent = false;
    s_vigilante_reinforcement_task_handle = NULL;
}

static bool orchestrator_is_vigilante_active(void)
{
    return s_active_webrtc_mode == WEBRTC_SESSION_MODE_VIGILANTE;
}

static void orchestrator_cancel_sleep_csi_cooldown(void)
{
    s_sleep_csi_generation++;
    s_sleep_motion_allowed_ms = 0;
}

static bool orchestrator_sleep_cooldown_active(void)
{
    return s_sleep_motion_allowed_ms != 0 &&
           (int32_t)(orchestrator_now_ms() - s_sleep_motion_allowed_ms) < 0;
}

static void orchestrator_sleep_csi_cooldown_task(void *param)
{
    uint32_t generation = (uint32_t)(uintptr_t)param;

    const uint32_t quiet_scan_delay_ms =
        (SLEEP_WIFI_READY_DISPLAY_MS < SLEEP_CSI_COOLDOWN_MS)
            ? SLEEP_WIFI_READY_DISPLAY_MS
            : SLEEP_CSI_COOLDOWN_MS;

    vTaskDelay(pdMS_TO_TICKS(quiet_scan_delay_ms));

    if (generation != s_sleep_csi_generation)
    {
        ESP_LOGD(TAG, "STATE_SLEEP: stale CSI cooldown task ignored");
        vTaskDelete(NULL);
        return;
    }

    orchestrator_show_phase("sleep_csi_cooldown", "Quiet scan", "Warming up", COLOR_CYAN_BGR565);

    if (SLEEP_CSI_COOLDOWN_MS > quiet_scan_delay_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(SLEEP_CSI_COOLDOWN_MS - quiet_scan_delay_ms));
    }

    if (generation == s_sleep_csi_generation)
    {
        ESP_LOGI(TAG, "STATE_SLEEP: Cooldown complete; starting unified motion sensing.");
        s_sleep_motion_allowed_ms = 0;
        
        if (g_hardware_radar_present) {
            ESP_LOGI(TAG, "STATE_SLEEP: Arming One-Shot Hardware Radar.");
            radar_hal_enable();
            orchestrator_show_phase("radar_watch", "Watching", "Radar Armed", COLOR_GREEN_BGR565);
        } else {
            esp_err_t csi_err = csi_handler_start();
            if (csi_err != ESP_OK)
            {
                ESP_LOGE(TAG, "STATE_SLEEP: failed to start CSI after cooldown: %s",
                         esp_err_to_name(csi_err));
            }
            else
            {
                orchestrator_show_phase("csi_watch", "Watching", "Motion scan", COLOR_GREEN_BGR565);
            }
        }
    }
    else
    {
        ESP_LOGD(TAG, "STATE_SLEEP: stale CSI cooldown task ignored");
    }

    vTaskDelete(NULL);
}

static void orchestrator_schedule_sleep_csi_start(void)
{
    uint32_t generation = ++s_sleep_csi_generation;
    s_sleep_motion_allowed_ms = orchestrator_now_ms() + SLEEP_CSI_COOLDOWN_MS;

    BaseType_t rc = xTaskCreate(orchestrator_sleep_csi_cooldown_task,
                                "csi_cooldown",
                                3072,
                                (void *)(uintptr_t)generation,
                                5,
                                NULL);
    if (rc != pdPASS)
    {
        ESP_LOGE(TAG, "STATE_SLEEP: failed to create CSI cooldown task");
    }
}

/**
 * @brief Take and log a heap memory snapshot.
 *
 * This function captures the current state of internal and PSRAM memory usage,
 * including free sizes, minimum free sizes, and largest available blocks.
 * The snapshot is formatted as a log message for easy analysis.
 *
 * @param stage A string label indicating the context or stage where the snapshot was taken
 */
static void orchestrator_log_heap_snapshot(const char *stage)
{
    const char *label = stage ? stage : "unknown";

    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    if (strcmp(label, "telemetry:periodic") == 0)
    {
        ESP_LOGD(TAG,
                 "[HEAP] %s | INTERNAL free=%zu min=%zu largest=%zu | DMA free=%zu largest=%zu | PSRAM free=%zu min=%zu largest=%zu",
                 label,
                 internal_free,
                 internal_min,
                 internal_largest,
                 dma_free,
                 dma_largest,
                 psram_free,
                 psram_min,
                 psram_largest);
    }
    else
    {
        ESP_LOGW(TAG,
                 "[HEAP] %s | INTERNAL free=%zu min=%zu largest=%zu | DMA free=%zu largest=%zu | PSRAM free=%zu min=%zu largest=%zu",
                 label,
                 internal_free,
                 internal_min,
                 internal_largest,
                 dma_free,
                 dma_largest,
                 psram_free,
                 psram_min,
                 psram_largest);
    }
}

static esp_err_t orchestrator_ensure_ui_ready(const char *reason)
{
    if (ui_is_initialized())
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restoring LCD UI after BLE handoff: %s", reason ? reason : "");
    orchestrator_log_heap_snapshot("ui_restore:before");
    esp_err_t err = ui_init();
    orchestrator_log_heap_snapshot((err == ESP_OK) ? "ui_restore:after" : "ui_restore:failed");
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD UI restore failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void orchestrator_show_phase(const char *reason,
                                    const char *title,
                                    const char *subtitle,
                                    uint16_t color)
{
    if (orchestrator_ensure_ui_ready(reason) == ESP_OK)
    {
        display_system_phase_message(title, subtitle, color);
    }
}

static void orchestrator_show_vigilante_alert_visual(void)
{
    esp_err_t err = orchestrator_ensure_ui_ready("dispatching_alert");
    if (err != ESP_OK)
    {
        return;
    }

    err = ui_simi_init();
    if (err == ESP_OK)
    {
        ui_simi_render_static(SIMI_STATE_ALERT);
        ESP_LOGW(TAG, "Vigilante Dr. Simi alert visual rendered");
        return;
    }

    ESP_LOGW(TAG, "Could not allocate Dr. Simi alert canvas: %s; using text fallback",
             esp_err_to_name(err));
    display_intruder_alert_message();
}

/**
 * @brief Ensure that the audio runtime is ready for use.
 *
 * This function checks if the audio runtime is already initialized and ready.
 * If not, it initializes the audio runtime by calling `media_sys_buildup()`
 * and then verifies that the runtime is indeed ready using `media_sys_is_ready()`.
 *
 * @note This function must be called before attempting to use audio-related
 *       features to prevent errors caused by an uninitialized audio runtime.
 *
 * @return `ESP_OK` if the audio runtime is ready, `ESP_FAIL` if initialization
 *         failed or the runtime is not ready after initialization.
 */
static esp_err_t orchestrator_ensure_audio_runtime_ready(void)
{
    if (media_sys_is_ready())
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

    return ESP_OK;
}

/**
 * @brief Check if the active session idle timeout has expired.
 *
 * This function determines if the current WebRTC session has been idle
 * for longer than the configured auto-sleep timeout. It considers both the
 * last activity timestamp and whether the WebRTC realtime interface is
 * currently busy.
 *
 * @note The auto-sleep timeout is defined by the `AUTO_SLEEP_TIMEOUT_MS`
 *       configuration constant.
 * @note The function returns `true` only when the session has been idle for
 *       the entire timeout duration and the WebRTC interface is not busy.
 *
 * @return `true` if the active session idle timeout has expired,
 *         `false` otherwise.
 */
static bool orchestrator_active_idle_expired(void)
{
    if (s_is_muted)
    {
        return false;
    }

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
    case STATE_PREPARING_BLE:
        return "STATE_PREPARING_BLE";
    case STATE_VALIDATING_IDENTITY:
        return "STATE_VALIDATING_IDENTITY";
    case STATE_RELEASING_BLE:
        return "STATE_RELEASING_BLE";
    case STATE_DISPATCHING_ALERT:
        return "STATE_DISPATCHING_ALERT";
    case STATE_IGNITING:
        return "STATE_IGNITING";
    case STATE_ACTIVE:
        return "STATE_ACTIVE";
    case STATE_AUTO_SLEEPING:
        return "STATE_AUTO_SLEEPING";
    case STATE_STOPPING_WEBRTC:
        return "STATE_STOPPING_WEBRTC";
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
    case ORCH_EVENT_BLE_READY:
        return "ORCH_EVENT_BLE_READY";
    case ORCH_EVENT_BLE_BUSY:
        return "ORCH_EVENT_BLE_BUSY";
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
    case ORCH_EVENT_WEBRTC_STOPPED:
        return "ORCH_EVENT_WEBRTC_STOPPED";
    case ORCH_EVENT_AUTO_SLEEP_TIMEOUT:
        return "ORCH_EVENT_AUTO_SLEEP_TIMEOUT";
    case ORCH_EVENT_ALERT_DISPATCH_COMPLETE:
        return "ORCH_EVENT_ALERT_DISPATCH_COMPLETE";
    case ORCH_EVENT_ALERT_DISPATCH_FAILED:
        return "ORCH_EVENT_ALERT_DISPATCH_FAILED";
    case ORCH_EVENT_VIGILANTE_ROOM_VACATED:
        return "ORCH_EVENT_VIGILANTE_ROOM_VACATED";
    case ORCH_EVENT_VIGILANTE_TIMEOUT:
        return "ORCH_EVENT_VIGILANTE_TIMEOUT";
    case ORCH_EVENT_MIC_MUTED:
        return "ORCH_EVENT_MIC_MUTED";
    case ORCH_EVENT_MIC_UNMUTED:
        return "ORCH_EVENT_MIC_UNMUTED";
    case ORCH_EVENT_IDLE_ALERT_START:
        return "ORCH_EVENT_IDLE_ALERT_START";
    case ORCH_EVENT_IDLE_ALERT_END:
        return "ORCH_EVENT_IDLE_ALERT_END";
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
    orchestrator_event_msg_t msg = {
        .type = event,
    };

    if (s_orchestrator_event_queue == NULL)
    {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping event=%d", event);
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(event));
    }
}

void orchestrator_post_mute_state(bool is_muted)
{
    orchestrator_event_t evt = is_muted ? ORCH_EVENT_MIC_MUTED : ORCH_EVENT_MIC_UNMUTED;
    orchestrator_post_event(evt);
}

bool orchestrator_get_mute_state(void)
{
    return s_is_muted;
}

void orchestrator_post_motion_detected(uint32_t timestamp_ms, float corr_drop)
{
    orchestrator_event_msg_t msg = {
        .type = ORCH_EVENT_MOTION_DETECTED,
        .timestamp_ms = timestamp_ms,
        .corr_drop = corr_drop,
    };

    if (s_orchestrator_event_queue == NULL)
    {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping motion event");
        return;
    }

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(msg.type));
    }
}

static void orchestrator_post_alert_dispatch_result(orchestrator_event_t event,
                                                    uint32_t timestamp_ms,
                                                    float corr_drop)
{
    if (s_orchestrator_event_queue == NULL)
    {
        ESP_LOGW(TAG, "Orchestrator queue not ready; dropping event=%d", event);
        return;
    }

    orchestrator_event_msg_t msg = {
        .type = event,
        .timestamp_ms = timestamp_ms,
        .corr_drop = corr_drop,
    };

    if (xQueueSend(s_orchestrator_event_queue,
                   &msg,
                   pdMS_TO_TICKS(ORCHESTRATOR_EVENT_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Orchestrator queue blocked; dropping %s", orchestrator_event_name(event));
    }
}

static void orchestrator_ble_prepare_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Cold-booting BLE stack for identity validation");
    orchestrator_log_heap_snapshot("ble_prepare:before");

    esp_err_t err = ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE pre-clean release failed: %s", esp_err_to_name(err));
        orchestrator_log_heap_snapshot("ble_prepare:preclean_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_common_ensure_ready(ble_sync_semaphore, false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE host cold-boot failed: %s", esp_err_to_name(err));
        ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        orchestrator_log_heap_snapshot("ble_prepare:host_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_device_control_start(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE central cold-boot failed: %s", esp_err_to_name(err));
        ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        orchestrator_log_heap_snapshot("ble_prepare:central_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_device_prepare_for_identity_scan(BLE_RELEASE_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE GAP scanner is not idle: %s", esp_err_to_name(err));
        ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        orchestrator_log_heap_snapshot("ble_prepare:gap_busy");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    orchestrator_log_heap_snapshot("ble_prepare:ready");
    orchestrator_post_event(ORCH_EVENT_BLE_READY);
    s_ble_prepare_task_handle = NULL;
    vTaskDelete(NULL);
}

static void orchestrator_start_ble_prepare(void)
{
    if (s_ble_prepare_task_handle != NULL)
    {
        ESP_LOGW(TAG, "BLE prepare already running");
        return;
    }

    BaseType_t rc = orchestrator_create_external_stack_task(orchestrator_ble_prepare_task,
                                                            "ble_prepare",
                                                            6144,
                                                            NULL,
                                                            6,
                                                            &s_ble_prepare_task_handle);
    if (rc != pdPASS)
    {
        s_ble_prepare_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BLE prepare task");
        orchestrator_log_heap_snapshot("ble_prepare:create_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
    }
}

/**
 * @brief Start bounded BLE identity validation after BLE has been prepared.
 *
 * BLE host initialization and central startup are handled by STATE_PREPARING_BLE.
 * Any failure here is a stack/lifecycle fault and must not be interpreted as
 * an unauthorized identity rejection.
 */
static void orchestrator_start_identity_validation(void)
{
    ESP_LOGI(TAG, "Starting bounded BLE identity validation (%d ms)", IDENTITY_VALIDATION_TIMEOUT_MS);
    orchestrator_log_heap_snapshot("identity_validation:start");

    esp_err_t err = ble_device_start_identity_validation(IDENTITY_VALIDATION_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE identity validation could not be scheduled: %s", esp_err_to_name(err));
        orchestrator_log_heap_snapshot("identity_validation:schedule_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        return;
    }

    orchestrator_log_heap_snapshot("identity_validation:scheduled");
}

/**
 * @brief Task to release BLE resources asynchronously.
 *
 * This function is executed in a separate task to release BLE resources
 * while the main thread continues with other operations. It logs the heap
 * state before and after the release and posts the appropriate event to the
 * orchestrator queue.
 *
 * @param param Task parameter (unused)
 */
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

/**
 * @brief Start the BLE release process.
 *
 * This function is called by the orchestrator to initiate the release of
 * BLE resources. It creates a dedicated task to handle the release process
 * asynchronously, allowing the main thread to continue with other operations.
 *
 * @note The BLE release is performed in a separate task to avoid blocking
 *       the main thread during the release operation.
 * @note If a BLE release is already in progress, this function will return
 *       early to prevent concurrent release operations.
 */
static void orchestrator_start_ble_release(void)
{
    if (s_ble_release_task_handle != NULL)
    {
        ESP_LOGW(TAG, "BLE release already running");
        return;
    }

    BaseType_t rc = orchestrator_create_external_stack_task(orchestrator_ble_release_task,
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

typedef struct
{
    uint32_t timestamp_ms;
    float corr_drop;
    bool reinforcement;
} alert_dispatch_task_ctx_t;

static void orchestrator_alert_dispatch_task(void *param)
{
    alert_dispatch_task_ctx_t *ctx = (alert_dispatch_task_ctx_t *)param;
    if (!ctx)
    {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = alert_dispatcher_send_alert(ctx->timestamp_ms, ctx->corr_drop);
    if (ctx->reinforcement)
    {
        if (err == ESP_OK)
        {
            ESP_LOGW(TAG, "Vigilante reinforcement alert dispatched");
        }
        else
        {
            ESP_LOGE(TAG, "Vigilante reinforcement alert failed: %s", esp_err_to_name(err));
        }
        s_vigilante_reinforcement_task_handle = NULL;
    }
    else
    {
        orchestrator_post_alert_dispatch_result((err == ESP_OK)
                                                    ? ORCH_EVENT_ALERT_DISPATCH_COMPLETE
                                                    : ORCH_EVENT_ALERT_DISPATCH_FAILED,
                                                ctx->timestamp_ms,
                                                ctx->corr_drop);
        s_alert_dispatch_task_handle = NULL;
    }

    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t orchestrator_start_alert_dispatch(uint32_t timestamp_ms,
                                                   float corr_drop,
                                                   bool reinforcement)
{
    TaskHandle_t *task_handle = reinforcement
                                    ? &s_vigilante_reinforcement_task_handle
                                    : &s_alert_dispatch_task_handle;
    if (*task_handle != NULL)
    {
        ESP_LOGW(TAG, "%s alert dispatch already running",
                 reinforcement ? "Reinforcement" : "Initial");
        return ESP_ERR_INVALID_STATE;
    }

    alert_dispatch_task_ctx_t *ctx = calloc(1, sizeof(alert_dispatch_task_ctx_t));
    if (!ctx)
    {
        ESP_LOGE(TAG, "Failed to allocate alert dispatch context");
        return ESP_ERR_NO_MEM;
    }

    ctx->timestamp_ms = timestamp_ms;
    ctx->corr_drop = corr_drop;
    ctx->reinforcement = reinforcement;

    BaseType_t rc = xTaskCreate(orchestrator_alert_dispatch_task,
                                reinforcement ? "alert_reinforce" : "alert_dispatch",
                                6144,
                                ctx,
                                5,
                                task_handle);
    if (rc != pdPASS)
    {
        *task_handle = NULL;
        free(ctx);
        ESP_LOGE(TAG, "Failed to create %s alert task",
                 reinforcement ? "reinforcement" : "initial");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void orchestrator_webrtc_stop_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "STATE_STOPPING_WEBRTC: closing WebRTC stack");
    orchestrator_log_heap_snapshot("webrtc_stop:before");
    int ret = stop_webrtc();
    if (ret != 0)
    {
        ESP_LOGW(TAG, "WebRTC stop returned %d; continuing shutdown barrier", ret);
    }
    orchestrator_log_heap_snapshot("webrtc_stop:after");

    orchestrator_post_event(ORCH_EVENT_WEBRTC_STOPPED);
    s_webrtc_stop_task_handle = NULL;
    vTaskDelete(NULL);
}

static void orchestrator_start_webrtc_stop(void)
{
    if (s_webrtc_stop_task_handle != NULL)
    {
        ESP_LOGW(TAG, "WebRTC stop already running");
        return;
    }

    s_webrtc_stop_started_ms = orchestrator_now_ms();
    BaseType_t rc = xTaskCreate(orchestrator_webrtc_stop_task,
                                "webrtc_stop",
                                WEBRTC_STOP_TASK_STACK_SIZE,
                                NULL,
                                WEBRTC_STOP_TASK_PRIORITY,
                                &s_webrtc_stop_task_handle);
    if (rc != pdPASS)
    {
        s_webrtc_stop_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create WebRTC stop task");
        orchestrator_post_event(ORCH_EVENT_WEBRTC_STOPPED);
    }
}

static bool orchestrator_webrtc_stop_timeout_expired(void)
{
    return s_webrtc_stop_started_ms != 0 &&
           (uint32_t)(orchestrator_now_ms() - s_webrtc_stop_started_ms) >= WEBRTC_STOP_TIMEOUT_MS;
}

static bool orchestrator_alert_result_matches(const orchestrator_event_msg_t *msg)
{
    return msg &&
           msg->timestamp_ms == s_alert_timestamp_ms &&
           msg->corr_drop == s_alert_corr_drop;
}

static void orchestrator_note_vigilante_motion_detected(const orchestrator_event_msg_t *msg)
{
    if (!orchestrator_is_vigilante_active())
    {
        return;
    }

    if (s_vigilante_resting_since_ms != 0)
    {
        ESP_LOGI(TAG,
                 "Vigilante resting window reset by motion event: corr_drop=%.4f timestamp=%lu",
                 msg ? msg->corr_drop : 0.0f,
                 (unsigned long)(msg ? msg->timestamp_ms : 0));
    }
    s_vigilante_resting_since_ms = 0;
}

static bool orchestrator_poll_vigilante_monitor(orchestrator_event_msg_t *out_msg)
{
    if (!out_msg || !orchestrator_is_vigilante_active())
    {
        return false;
    }

    const uint32_t now_ms = orchestrator_now_ms();
    if (s_vigilante_active_started_ms == 0)
    {
        s_vigilante_active_started_ms = now_ms;
    }

    if ((uint32_t)(now_ms - s_vigilante_active_started_ms) >= VIGILANTE_ACTIVE_TIMEOUT_MS)
    {
        out_msg->type = ORCH_EVENT_VIGILANTE_TIMEOUT;
        return true;
    }

    ei_inference_status_t status = {0};
    ei_inference_get_status(&status);
    const bool status_fresh = status.valid &&
                              (uint32_t)(now_ms - status.updated_ms) <= VIGILANTE_STATUS_STALE_MS;

    if (!s_vigilante_reinforcement_sent &&
        (uint32_t)(now_ms - s_vigilante_active_started_ms) >= VIGILANTE_REINFORCEMENT_DELAY_MS)
    {
        s_vigilante_reinforcement_sent = true;
        if (status_fresh && status.motion_active)
        {
            ESP_LOGW(TAG, "Vigilante motion persists after %d ms; dispatching reinforcement alert",
                     VIGILANTE_REINFORCEMENT_DELAY_MS);
            esp_err_t err = orchestrator_start_alert_dispatch(now_ms,
                                                              status.corr_drop,
                                                              true);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to start reinforcement alert: %s", esp_err_to_name(err));
            }
        }
        else
        {
            ESP_LOGI(TAG, "Vigilante reinforcement skipped; motion is not active");
        }
    }

    if (status_fresh && status.resting)
    {
        if (s_vigilante_resting_since_ms == 0)
        {
            s_vigilante_resting_since_ms = now_ms;
        }
        else if ((uint32_t)(now_ms - s_vigilante_resting_since_ms) >= VIGILANTE_VACATED_CONFIRM_MS)
        {
            if (webrtc_realtime_is_busy())
            {
                ESP_LOGI(TAG, "Vigilante room-vacated candidate suppressed while WebRTC audio/session is busy");
                s_vigilante_resting_since_ms = now_ms;
                return false;
            }

            out_msg->type = ORCH_EVENT_VIGILANTE_ROOM_VACATED;
            out_msg->timestamp_ms = status.updated_ms;
            out_msg->corr_drop = status.corr_drop;
            return true;
        }
    }
    else
    {
        s_vigilante_resting_since_ms = 0;
    }

    return false;
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
        orchestrator_cancel_sleep_csi_cooldown();
        reset_alert_context();
        reset_vigilante_runtime_context();
        s_ble_release_to_sleep = false;
        radar_hal_disable();
        csi_handler_stop();
        ui_simi_stop();
        ui_simi_deinit();
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
        reset_alert_context();
        reset_vigilante_runtime_context();
        s_ble_release_to_sleep = false;
        s_webrtc_stop_started_ms = 0;
        radar_hal_disable();
        csi_handler_stop();
        ui_simi_stop();
        ui_simi_deinit();
        orchestrator_show_phase("wifi_ready", "WiFi ready", "Standing by", COLOR_GREEN_BGR565);
        esp_err_t sleep_ble_err = ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        if (sleep_ble_err != ESP_OK)
        {
            ESP_LOGE(TAG, "STATE_SLEEP: BLE full release failed; continuing CSI recovery path: %s",
                     esp_err_to_name(sleep_ble_err));
            orchestrator_log_heap_snapshot("sleep:ble_release_failed");
        }
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT | WIFI_DISCONNECTED_BIT);
        ESP_LOGI(TAG, "STATE_SLEEP: waiting %d ms before enabling CSI.", SLEEP_CSI_COOLDOWN_MS);
        orchestrator_schedule_sleep_csi_start();
        break;
    }

    case STATE_PREPARING_BLE:
        ESP_LOGI(TAG, "STATE_PREPARING_BLE: pausing CSI and cold-booting BLE for identity validation.");
        orchestrator_cancel_sleep_csi_cooldown();
        radar_hal_disable();
        csi_handler_stop();
        ui_simi_stop();
        ui_simi_deinit();
        orchestrator_show_phase("ble_prepare_status", "Checking ID", "Stay close", COLOR_YELLOW_BGR565);
        esp_err_t ui_deinit_err = ui_deinit_keep_last_frame();
        if (ui_deinit_err != ESP_OK)
        {
            ESP_LOGW(TAG, "STATE_PREPARING_BLE: LCD release returned %s",
                     esp_err_to_name(ui_deinit_err));
        }
        orchestrator_log_heap_snapshot("ble_prepare:after_ui_release");
        orchestrator_start_ble_prepare();
        break;

    case STATE_VALIDATING_IDENTITY:
        ESP_LOGI(TAG, "STATE_VALIDATING_IDENTITY: BLE ready; starting identity validation scan.");
        orchestrator_cancel_sleep_csi_cooldown();
        radar_hal_disable();
        csi_handler_stop();
        orchestrator_start_identity_validation();

        // --- INICIO DE SIMULACIÓN TEMPORAL ---
        // ESP_LOGW(TAG, "SIMULACIÓN: Forzando identidad validada para probar WebRTC...");
        // orchestrator_post_event(ORCH_EVENT_IDENTITY_PRESENT);
        // --- FIN DE SIMULACIÓN TEMPORAL ---
        break;

    case STATE_RELEASING_BLE:
        ESP_LOGI(TAG, "STATE_RELEASING_BLE: releasing NimBLE before audio ignition.");
        orchestrator_cancel_sleep_csi_cooldown();
        radar_hal_disable();
        csi_handler_stop();
        orchestrator_start_ble_release();
        break;

    case STATE_DISPATCHING_ALERT:
    {
        ESP_LOGW(TAG,
                 "STATE_DISPATCHING_ALERT: dispatching emergency alert after BLE release.");
        orchestrator_cancel_sleep_csi_cooldown();
        radar_hal_disable();
        csi_handler_stop();
        orchestrator_show_vigilante_alert_visual();
        if (!s_alert_dispatch_pending)
        {
            ESP_LOGW(TAG, "STATE_DISPATCHING_ALERT entered without pending alert context");
            orchestrator_post_alert_dispatch_result(ORCH_EVENT_ALERT_DISPATCH_FAILED,
                                                    s_alert_timestamp_ms,
                                                    s_alert_corr_drop);
            break;
        }

        esp_err_t alert_err = orchestrator_start_alert_dispatch(s_alert_timestamp_ms,
                                                                s_alert_corr_drop,
                                                                false);
        if (alert_err != ESP_OK)
        {
            ESP_LOGE(TAG, "STATE_DISPATCHING_ALERT: failed to start alert task: %s",
                     esp_err_to_name(alert_err));
            orchestrator_post_alert_dispatch_result(ORCH_EVENT_ALERT_DISPATCH_FAILED,
                                                    s_alert_timestamp_ms,
                                                    s_alert_corr_drop);
        }
        break;
    }

    case STATE_IGNITING:
    {
        ESP_LOGI(TAG, "STATE_IGNITING: initializing audio runtime and starting WebRTC.");
        orchestrator_cancel_sleep_csi_cooldown();
        radar_hal_disable();
        csi_handler_stop();
        s_arrival_context_sent = false;
        webrtc_session_mode_t ignition_mode = s_ignition_webrtc_mode;
        s_active_webrtc_mode = ignition_mode;

        orchestrator_log_heap_snapshot("igniting:before_audio");
        
        // Ensure UI is ready and show Welcome Screen before audio loads
        if (orchestrator_ensure_ui_ready("igniting") == ESP_OK)
        {
            if (ignition_mode != WEBRTC_SESSION_MODE_VIGILANTE)
            {
                display_welcome_identity(ble_identity_get_last_validated_name());
            }
        }

        esp_err_t audio_err = orchestrator_ensure_audio_runtime_ready();
        if (audio_err != ESP_OK || !media_sys_is_ready())
        {
            ESP_LOGE(TAG, "STATE_IGNITING: audio runtime not ready; aborting WebRTC start");
            orchestrator_log_heap_snapshot("igniting:audio_failed");
            orchestrator_post_event(ORCH_EVENT_WEBRTC_DISCONNECTED);
            break;
        }
        orchestrator_log_heap_snapshot("igniting:audio_ready");

        int ret = start_webrtc(ignition_mode);
        s_ignition_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
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
        orchestrator_cancel_sleep_csi_cooldown();
        if (s_is_muted && media_sys_is_ready())
        {
            ESP_LOGI(TAG, "STATE_ACTIVE: Re-applying persisted mute state.");
            media_sys_mic_mute(true);
            ui_simi_set_state(SIMI_STATE_MUTED);
            ui_show_status_message("Muted - Sleeping", COLOR_RED_BGR565);
        }
        if (orchestrator_is_vigilante_active())
        {
            ESP_LOGW(TAG, "STATE_ACTIVE: Vigilante Mode active; starting CSI threat monitor.");
            s_vigilante_active_started_ms = orchestrator_now_ms();
            s_vigilante_resting_since_ms = 0;
            s_vigilante_reinforcement_sent = false;
            esp_err_t csi_err = csi_handler_start();
            if (csi_err != ESP_OK)
            {
                ESP_LOGE(TAG, "STATE_ACTIVE: failed to start Vigilante CSI monitor: %s",
                         esp_err_to_name(csi_err));
            }
        }
        else
        {
            radar_hal_disable();
            csi_handler_stop();
        }
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
        ESP_LOGI(TAG, "STATE_AUTO_SLEEPING: preparing deterministic WebRTC shutdown.");
        orchestrator_cancel_sleep_csi_cooldown();
        s_arrival_context_sent = false;
        radar_hal_disable();
        csi_handler_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT);
        break;

    case STATE_STOPPING_WEBRTC:
        ESP_LOGI(TAG, "STATE_STOPPING_WEBRTC: waiting for explicit WebRTC stopped event.");
        orchestrator_cancel_sleep_csi_cooldown();
        s_arrival_context_sent = false;
        radar_hal_disable();
        csi_handler_stop();
        xEventGroupClearBits(app_startup_event_group,
                             WEBRTC_CONNECTED_BIT | WEBRTC_DISCONNECTED_BIT |
                                 WEBRTC_API_ERROR_BIT);
        orchestrator_start_webrtc_stop();
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
    orchestrator_event_msg_t event_msg;

    ESP_LOGI(TAG, "Orchestrator state machine started in %s", orchestrator_state_name(state));

    while (1)
    {
        TickType_t wait_ticks = (state == STATE_ACTIVE || state == STATE_STOPPING_WEBRTC)
                                    ? pdMS_TO_TICKS(AUTO_SLEEP_POLL_MS)
                                    : (s_aht30_present ? pdMS_TO_TICKS(10000) : portMAX_DELAY);

        if (xQueueReceive(s_orchestrator_event_queue, &event_msg, wait_ticks) != pdTRUE)
        {
            memset(&event_msg, 0, sizeof(event_msg));
            if (state == STATE_STOPPING_WEBRTC &&
                orchestrator_webrtc_stop_timeout_expired())
            {
                ESP_LOGE(TAG,
                         "STATE_STOPPING_WEBRTC: hard timeout after %d ms; forcing sleep",
                         WEBRTC_STOP_TIMEOUT_MS);
                orchestrator_enter_state(&state, STATE_SLEEP);
                continue;
            }
            else if (state == STATE_ACTIVE &&
                     orchestrator_is_vigilante_active() &&
                     orchestrator_poll_vigilante_monitor(&event_msg))
            {
                /* event_msg was filled by the monitor. */
            }
            else if (state == STATE_ACTIVE &&
                     !orchestrator_is_vigilante_active() &&
                     orchestrator_active_idle_expired())
            {
                event_msg.type = ORCH_EVENT_AUTO_SLEEP_TIMEOUT;
            }
            else
            {
                if (s_aht30_present) {
                    uint32_t now = orchestrator_now_ms();
                    if (now - s_last_aht30_poll_ms >= 600000) {
                        poll_and_draw_temperature();
                        s_last_aht30_poll_ms = now;
                    }
                }
                continue;
            }
        }

        orchestrator_event_t event = event_msg.type;
        ESP_LOGI(TAG, "Orchestrator event: %s while in %s",
                 orchestrator_event_name(event),
                 orchestrator_state_name(state));

        if (s_aht30_present && (event == ORCH_EVENT_WEBRTC_CONNECTED || event == ORCH_EVENT_WEBRTC_DISCONNECTED)) {
            poll_and_draw_temperature();
            s_last_aht30_poll_ms = orchestrator_now_ms();
        }

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
                if (orchestrator_sleep_cooldown_active())
                {
                    ESP_LOGW(TAG, "Ignoring motion event during %d ms STATE_SLEEP cooldown",
                             SLEEP_CSI_COOLDOWN_MS);
                    break;
                }
                s_alert_timestamp_ms = event_msg.timestamp_ms ? event_msg.timestamp_ms : orchestrator_now_ms();
                s_alert_corr_drop = event_msg.corr_drop;
                s_alert_dispatch_pending = false;
                s_pending_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ble_release_to_sleep = false;
                orchestrator_show_phase("motion_detected", "Motion seen", "Checking ID", COLOR_YELLOW_BGR565);
                orchestrator_enter_state(&state, STATE_PREPARING_BLE);
            }
            else if (event != ORCH_EVENT_WIFI_CONNECTED)
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_PREPARING_BLE:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_BLE_READY)
            {
                orchestrator_enter_state(&state, STATE_VALIDATING_IDENTITY);
            }
            else if (event == ORCH_EVENT_BLE_BUSY)
            {
                ESP_LOGE(TAG, "BLE preparation failed; returning to sleep without alert dispatch");
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
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
                s_alert_dispatch_pending = false;
                s_pending_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ignition_webrtc_mode = WEBRTC_SESSION_MODE_FRIENDLY;
                s_ble_release_to_sleep = false;
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            }
            else if (event == ORCH_EVENT_IDENTITY_REJECTED)
            {
                if (s_alert_timestamp_ms == 0)
                {
                    s_alert_timestamp_ms = orchestrator_now_ms();
                }
                s_alert_dispatch_pending = true;
                s_pending_webrtc_mode = WEBRTC_SESSION_MODE_VIGILANTE;
                s_ble_release_to_sleep = false;

                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
            }
            else if (event == ORCH_EVENT_BLE_BUSY)
            {
                ESP_LOGE(TAG, "BLE validation stack fault; releasing BLE and returning to sleep without alert dispatch");
                reset_alert_context();
                s_ble_release_to_sleep = true;
                orchestrator_enter_state(&state, STATE_RELEASING_BLE);
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
                if (s_ble_release_to_sleep)
                {
                    s_ble_release_to_sleep = false;
                    orchestrator_enter_state(&state, STATE_SLEEP);
                }
                else if (s_alert_dispatch_pending)
                {
                    orchestrator_enter_state(&state, STATE_DISPATCHING_ALERT);
                }
                else
                {
                    s_ignition_webrtc_mode = s_pending_webrtc_mode;
                    orchestrator_enter_state(&state, STATE_IGNITING);
                }
            }
            else if (event == ORCH_EVENT_BLE_RELEASE_FAILED)
            {
                orchestrator_log_heap_snapshot("ble_release:failed_to_sleep");
                s_ble_release_to_sleep = false;
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else
            {
                orchestrator_ignore_event(state, event);
            }
            break;

        case STATE_DISPATCHING_ALERT:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_ALERT_DISPATCH_COMPLETE ||
                     event == ORCH_EVENT_ALERT_DISPATCH_FAILED)
            {
                if (!orchestrator_alert_result_matches(&event_msg))
                {
                    ESP_LOGW(TAG, "Ignoring stale alert dispatch result");
                    break;
                }

                if (event == ORCH_EVENT_ALERT_DISPATCH_FAILED)
                {
                    ESP_LOGW(TAG, "Initial alert dispatch failed; continuing to Vigilante ignition");
                }
                s_ignition_webrtc_mode = s_pending_webrtc_mode;
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_IGNITING);
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
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
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
            else if (event == ORCH_EVENT_MIC_MUTED || event == ORCH_EVENT_MIC_UNMUTED)
            {
                bool want_mute = (event == ORCH_EVENT_MIC_MUTED);
                if (s_is_muted != want_mute)
                {
                    s_is_muted = want_mute;
                    if (media_sys_is_ready())
                    {
                        media_sys_mic_mute(s_is_muted);
                    }
                }
                
                // Synchronize UI Canvas State
                if (event == ORCH_EVENT_MIC_MUTED)
                {
                    mute_handler_start_idle_timer();
                    ui_simi_set_state(SIMI_STATE_MUTED);
                    ui_show_status_message("Muted - Sleeping", COLOR_RED_BGR565);
                }
                else
                {
                    mute_handler_stop_idle_timer();
                    ui_simi_set_state(SIMI_STATE_LISTENING);
                    ui_clear_status_message();
                    webrtc_post_action(WEBRTC_ACTION_NOTIFY_UNMUTE);
                }
            }
            else if (event == ORCH_EVENT_IDLE_ALERT_START)
            {
                ui_clear_status_message();
                ui_show_status_message("You there?", COLOR_YELLOW_BGR565);
            }
            else if (event == ORCH_EVENT_IDLE_ALERT_END)
            {
                if (s_is_muted)
                {
                    ui_show_status_message("Muted - Sleeping", COLOR_RED_BGR565);
                }
                else
                {
                    ui_clear_status_message();
                    ui_simi_set_state(SIMI_STATE_LISTENING);
                }
            }
            else if (event == ORCH_EVENT_AUTO_SLEEP_TIMEOUT &&
                     !orchestrator_is_vigilante_active())
            {
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            }
            else if (event == ORCH_EVENT_VIGILANTE_ROOM_VACATED ||
                     event == ORCH_EVENT_VIGILANTE_TIMEOUT)
            {
                ESP_LOGW(TAG, "Vigilante lifecycle ended by %s", orchestrator_event_name(event));
                reset_alert_context();
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            }
            else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                     event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                orchestrator_enter_state(&state, STATE_AUTO_SLEEPING);
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            }
            else if (event == ORCH_EVENT_MOTION_DETECTED)
            {
                orchestrator_note_vigilante_motion_detected(&event_msg);
                orchestrator_ignore_event(state, event);
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
                orchestrator_enter_state(&state, STATE_STOPPING_WEBRTC);
            }
            break;

        case STATE_STOPPING_WEBRTC:
            if (event == ORCH_EVENT_WIFI_DISCONNECTED)
            {
                orchestrator_enter_state(&state, STATE_WAIT_WIFI);
            }
            else if (event == ORCH_EVENT_WEBRTC_STOPPED)
            {
                s_webrtc_stop_started_ms = 0;
                orchestrator_enter_state(&state, STATE_SLEEP);
            }
            else if (event == ORCH_EVENT_WEBRTC_DISCONNECTED ||
                     event == ORCH_EVENT_WEBRTC_API_ERROR)
            {
                ESP_LOGI(TAG, "Ignoring WebRTC disconnect notification during local shutdown");
            }
            else
            {
                orchestrator_ignore_event(state, event);
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
    esp_log_level_set("CODEC_INIT", ESP_LOG_WARN);
    esp_log_level_set("ES7210", ESP_LOG_WARN);
    esp_log_level_set("ES8311", ESP_LOG_WARN);
    esp_log_level_set("I2S_IF", ESP_LOG_WARN);
    esp_log_level_set("AGENT", ESP_LOG_WARN);
    esp_log_level_set("SCTP", ESP_LOG_WARN);
    esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
    esp_log_level_set("webrtc", ESP_LOG_WARN);
    esp_log_level_set("AV_RENDER", ESP_LOG_WARN);

    esp_err_t heap_hook_err = heap_caps_register_failed_alloc_callback(app_heap_alloc_failed_hook);
    if (heap_hook_err != ESP_OK)
    {
        ESP_LOGW(TAG, "No se pudo registrar hook de fallo de heap: %s", esp_err_to_name(heap_hook_err));
    }

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
    s_orchestrator_event_queue = xQueueCreate(ORCHESTRATOR_QUEUE_DEPTH, sizeof(orchestrator_event_msg_t));
    if (app_startup_event_group == NULL || ble_sync_semaphore == NULL || s_orchestrator_event_queue == NULL)
    {
        ESP_LOGE(TAG, "No se pudieron crear primitivas de sincronizacion de arranque");
        return;
    }

    // --- BARE-METAL RADAR HAL INIT ---
    if (radar_hal_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Radar HAL");
    }
    // ---------------------------------

    // 5) BLE se inicializa bajo demanda para no competir con el primer intento WiFi.
    ESP_LOGI(TAG, "BLE se inicializara bajo demanda.");
    ESP_LOGI(TAG, "BLE Central permanece deshabilitado por defecto.");

    // 6) Iniciar tarea orquestadora de arranque
    xTaskCreate(app_startup_orchestrator_task, "startup_orch", 4096, NULL, 5, NULL);

    aht30_init_once();

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
        if (s_aht30_present) {
            poll_and_draw_temperature();
            s_last_aht30_poll_ms = orchestrator_now_ms();
        }
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
