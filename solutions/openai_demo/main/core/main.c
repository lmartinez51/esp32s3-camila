/**
 * @file main.c
 * @brief Main application entry point for ESP32-S3-BOX3 AI Chatbot using OpenAI Realtime API.
 *
 * This file is intentionally thin. Its sole responsibility is:
 *   1. Configuring log levels.
 *   2. Initializing hardware and OS primitives.
 *   3. Creating the orchestrator task and launching the WiFi/BLE bootstrap.
 *   4. Running the idle main-loop that queries WebRTC status.
 *
 * All orchestrator logic is split across the sibling modules in core/:
 *   - sensor_dock.c       — AHT30 sensor and dock detection
 *   - orchestrator_helpers.c — shared state, heap logging, UI/audio helpers
 *   - orchestrator_tasks.c   — async FreeRTOS tasks (BLE, alert, WebRTC stop)
 *   - orchestrator_vigilante.c — Vigilante-mode presence monitor
 *   - orchestrator_fsm.c     — state machine transitions and orchestrator task
 *
 * @note This project is based on the ESP WebRTC solution from Espressif:
 *       https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/openai_demo
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

/* ── Standard / ESP-IDF Includes ────────────────────────────────────────── */
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <nvs_flash.h>
#include "esp_littlefs.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"

/* ── WebRTC and Media ───────────────────────────────────────────────────── */
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"

/* ── Project Modules ────────────────────────────────────────────────────── */
#include "hardware/radar.h"
#include "common.h"
#include "lua_benchmark.h"
#include "esp_claw_init.h"
#include "ble_config.h"
#include "ble_common.h"
#include "codec_init.h"
#include "ui.h"
#include "simi.h"
#include "mute_handler.h"
#include "media_sys.h"
#include "bsp/esp-bsp.h"
#include "nvs_setup.h"
#include "ble_device_callbacks.h"
#include "ble_device_control.h"
#include "responses_client.h"
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
#include "hardware/ir_sniffer.h"

/* ── Orchestrator Sub-Modules ───────────────────────────────────────────── */
#include "sensor_dock.h"
#include "orchestrator_helpers.h"
#include "orchestrator_tasks.h"
#include "orchestrator_vigilante.h"
#include "orchestrator_fsm.h"

static const char *TAG = "MAIN";

/* ── Compile-time Feature Flags ─────────────────────────────────────────── */

/** Set to 1 only during first-boot NVS provisioning runs. */
#define ENABLE_ONE_TIME_PROVISIONING 0

/* ── Thread Scheduler ───────────────────────────────────────────────────── */

/**
 * @brief Macro to execute tasks asynchronously in separate threads.
 *
 * Creates a temporary function that executes the given body asynchronously
 * and automatically destroys the thread when finished.
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
 * based on thread name to optimize system performance for real-time
 * audio/video processing. Different threads are assigned to different CPU
 * cores for load balancing.
 *
 * @param thread_name Name of the thread to configure
 * @param thread_cfg  Thread configuration structure to populate
 */
static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *thread_cfg)
{
    /* WebRTC peer connection task — Core 1, high priority */
    if (strcmp(thread_name, "pc_task") == 0) {
        thread_cfg->stack_size = 24 * 1024;
        thread_cfg->priority   = 18;
        thread_cfg->core_id    = 1;
    }
    /* WebRTC data sending task — Core 1, medium-high priority */
    if (strcmp(thread_name, "pc_send") == 0) {
        thread_cfg->stack_size = 3 * 1024;
        thread_cfg->priority   = 15;
        thread_cfg->core_id    = 1;
    }
    /* Audio decoder task — Core 1, medium priority */
    if (strcmp(thread_name, "Adec") == 0) {
        thread_cfg->stack_size = 24 * 1024;
        thread_cfg->priority   = 10;
        thread_cfg->core_id    = 1;
    }
    /* Video encoder task */
    if (strcmp(thread_name, "venc") == 0) {
#if CONFIG_IDF_TARGET_ESP32S3
        thread_cfg->stack_size = 20 * 1024;
#endif
        thread_cfg->priority = 10;
    }
#ifdef WEBRTC_SUPPORT_OPUS
    /* Audio encoder task — only when OPUS is enabled */
    if (strcmp(thread_name, "aenc") == 0) {
        thread_cfg->stack_size = 40 * 1024;
        thread_cfg->priority   = 10;
    }
    /* Audio source reading task — Core 0, high priority */
    if (strcmp(thread_name, "SrcRead") == 0) {
        thread_cfg->stack_size = 40 * 1024;
        thread_cfg->priority   = 16;
        thread_cfg->core_id    = 0;
    }
    /* Audio buffer input task — Core 0, medium priority */
    if (strcmp(thread_name, "buffer_in") == 0) {
        thread_cfg->stack_size = 6 * 1024;
        thread_cfg->priority   = 10;
        thread_cfg->core_id    = 0;
    }
#endif
}

/* ── BLE Provisioning Helper ────────────────────────────────────────────── */

/**
 * @brief Start BLE provisioning mode with step-by-step logging.
 *
 * Ensures BLE is ready, acquires provisioning ownership, and starts
 * advertising for WiFi provisioning. Handles errors gracefully.
 *
 * @param reason Optional description of why provisioning is starting (for logging).
 */
static void start_ble_provisioning_mode(const char *reason)
{
    ESP_LOGI(TAG, "Activando BLE provisioning: %s", reason ? reason : "");

    esp_err_t err = ble_common_ensure_ready(ble_sync_semaphore, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo preparar BLE provisioning: %s", esp_err_to_name(err));
        return;
    }

    err = ble_common_acquire(BLE_COMMON_ROLE_PROVISIONING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo adquirir BLE provisioning ownership: %s", esp_err_to_name(err));
        return;
    }

    ble_wifi_start_advertising();
}

/* ── Network Event Handler ──────────────────────────────────────────────── */

/**
 * @brief Network event handler for WiFi connection state changes.
 *
 * Handles network connectivity events by updating event-group bits and
 * notifying the orchestrator state machine. WebRTC is never started here.
 *
 * @param connected True if network is connected, false if disconnected.
 * @return 0 on success, non-zero on error.
 */
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
        xEventGroupClearBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_CONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WIFI_CONNECTED);
    }
    else
    {
        ESP_LOGI(TAG, "Evento de Red: WiFi Desconectado.");
        xEventGroupClearBits(app_startup_event_group, WIFI_CONNECTED_BIT | WEBRTC_CONNECTED_BIT);
        xEventGroupSetBits(app_startup_event_group, WIFI_DISCONNECTED_BIT);
        orchestrator_post_event(ORCH_EVENT_WIFI_DISCONNECTED);
    }
    return 0;
}

/* ── Telemetry Task ─────────────────────────────────────────────────────── */

/**
 * @brief Diagnostic telemetry task for Phase 0 Baseline.
 *
 * Periodically logs Internal SRAM and PSRAM statistics to detect memory
 * leaks or fragmentation during active WebRTC sessions.
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

/* ── Heap Alloc-Fail Hook ───────────────────────────────────────────────── */

static void app_heap_alloc_failed_hook(size_t      requested_size,
                                       uint32_t    caps,
                                       const char *function_name)
{
    ESP_EARLY_LOGE(TAG,
                   "[HEAP] alloc_failed | size=%u caps=0x%08" PRIx32 " fn=%s"
                   " | INTERNAL free=%u largest=%u"
                   " | DMA free=%u largest=%u"
                   " | PSRAM free=%u largest=%u",
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

/* ── Application Entry Point ────────────────────────────────────────────── */

/**
 * @brief Main application entry point.
 *
 * Initializes all system components in the following order:
 *  1. Log levels + heap hook
 *  2. NVS and UI
 *  3. I2C, media libraries, board init
 *  4. Mute handler, config manager, HTTP worker, WebRTC action queue
 *  5. Synchronization primitives (event group, semaphore, orchestrator queue)
 *  6. Radar HAL, sensor dock (AHT30), IR sniffer
 *  7. Orchestrator task
 *  8. WiFi connect (or BLE provisioning if WiFi fails)
 *  9. Telemetry task + idle main loop
 */
void app_main(void)
{
    /* ── Log Level Configuration ─────────────────────────────────────── */
    esp_log_level_set(TAG,         ESP_LOG_INFO);
    esp_log_level_set("CODEC_INIT", ESP_LOG_WARN);
    esp_log_level_set("ES7210",     ESP_LOG_WARN);
    esp_log_level_set("ES8311",     ESP_LOG_WARN);
    esp_log_level_set("I2S_IF",     ESP_LOG_WARN);
    esp_log_level_set("AGENT",      ESP_LOG_WARN);
    esp_log_level_set("SCTP",       ESP_LOG_WARN);
    esp_log_level_set("PEER_DEF",   ESP_LOG_WARN);
    esp_log_level_set("webrtc",     ESP_LOG_WARN);
    esp_log_level_set("AV_RENDER",  ESP_LOG_WARN);

    esp_err_t heap_hook_err = heap_caps_register_failed_alloc_callback(app_heap_alloc_failed_hook);
    if (heap_hook_err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo registrar hook de fallo de heap: %s",
                 esp_err_to_name(heap_hook_err));
    }

    /* ── Pre-boot: NVS ───────────────────────────────────────────────── */
    init_nvs();

    /* 1) UI */
    esp_err_t err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falló la inicialización de la UI: %s", esp_err_to_name(err));
        return;
    }

    /* 2) I2C bus, media adapter, thread scheduler */
    bsp_i2c_init();
    media_lib_add_default_adapter();
    media_lib_thread_set_schedule_cb(thread_scheduler);

    /* 3) Mute handler */
    mute_handler_init();

    bool boot_to_provisioning = nvs_read_and_clear_boot_to_provisioning_flag();

#if ENABLE_ONE_TIME_PROVISIONING
    const char *current_ssid = "example-ssid";
    // nvs_provision_known_profiles();
    // nvs_provision_hue_test_device(current_ssid);
    debug_nvs_contents(current_ssid);
    list_all_ble_devices_from_nvs();
    list_all_characteristics_from_nvs();
    esp_err_t error = nvs_delete_api_key();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "API Key eliminada de NVS");
    } else {
        ESP_LOGE(TAG, "Error al eliminar: %s", esp_err_to_name(error));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    list_api_keys_from_nvs();
#endif

    /* 4) Config manager, HTTP worker, WebRTC action queue */
    config_manager_init();
    http_worker_init();
    webrtc_init_action_queue();

    /* 5) Synchronization primitives */
    app_startup_event_group   = xEventGroupCreate();
    ble_sync_semaphore        = xSemaphoreCreateBinary();
    s_orchestrator_event_queue = xQueueCreate(ORCHESTRATOR_QUEUE_DEPTH,
                                              sizeof(orchestrator_event_msg_t));
    if (app_startup_event_group == NULL ||
        ble_sync_semaphore      == NULL ||
        s_orchestrator_event_queue == NULL)
    {
        ESP_LOGE(TAG, "No se pudieron crear primitivas de sincronizacion de arranque");
        return;
    }

    /* 6) Radar HAL */
    if (radar_hal_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Radar HAL");
    }

    ESP_LOGI(TAG, "BLE se inicializara bajo demanda.");
    ESP_LOGI(TAG, "BLE Central permanece deshabilitado por defecto.");

    /* 7) Orchestrator task */
    xTaskCreate(app_startup_orchestrator_task, "startup_orch", 4096, NULL, 5, NULL);

    /* 8) Sensor dock (AHT30 + dock detection) + IR sniffer */
    sensor_dock_init();

    if (s_aht30_present) {
        ESP_LOGI(TAG, "Dock confirmed. Initializing IR Sniffer on GPIO 38.");
        ir_sniffer_init();
        // esp_claw_init();
        // ir_sniffer_enter_pairing_mode(IR_ACTION_OUTFIT_BARCA);
    } else {
        ESP_LOGW(TAG, "Dock not present. Bypassing IR Sniffer to prevent floating noise.");
    }

    /* 9) WiFi connect or BLE provisioning */
    bool wifi_connected = false;
    if (boot_to_provisioning)
    {
        ESP_LOGW(TAG, "Mostrando pantalla de Modo Configuración en arranque.");
        display_config_mode_message();
    }
    else
    {
        ESP_LOGI(TAG, "Inicializando WiFi. WebRTC queda bajo demanda.");

        if (esp_claw_init() == ESP_OK) {
            ESP_LOGI(TAG, "Isolated Lua VM spawned successfully.");
        }

        display_startup_screen();
        if (s_aht30_present) {
            sensor_dock_poll_temperature();
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
        if (!boot_to_provisioning)
        {
            ESP_LOGI(TAG, "WiFi no conectado (fallo normal). Mostrando pantalla de credenciales WiFi.");
            display_wifi_creds();
        }
        else
        {
            ESP_LOGW(TAG, "WiFi no conectado (forzado por bandera). Pantalla 'Config Mode' ya visible.");
        }
        start_ble_provisioning_mode(boot_to_provisioning
                                        ? "Arranque forzado a provisioning."
                                        : "WiFi agoto reintentos.");
    }

    /* 10) Baseline telemetry task (Core 1, low priority, PSRAM stack) */
    xTaskCreatePinnedToCoreWithCaps(sys_telemetry_task, "telemetry_task", 3072, NULL,
                                    tskIDLE_PRIORITY + 1, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* Safe to unblock Lua VM now that all hardware and WiFi initializations are done */
    esp_claw_signal_safe_to_start();

    /* 11) Idle main loop */
    while (1)
    {
        media_lib_thread_sleep(2000);
        query_webrtc();
    }
}
