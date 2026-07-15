/**
 * @file orchestrator_tasks.c
 * @brief Asynchronous FreeRTOS task implementations for the orchestrator.
 *
 * Each task in this module is short-lived (fire-and-forget) and posts a
 * result event back to the orchestrator queue when complete. Tasks are
 * created via orchestrator_create_external_stack_task() so their stacks
 * can reside in PSRAM when the build configuration allows it.
 *
 * Modules:
 *   - BLE prepare task   (cold-boots the BLE host + GAP scanner)
 *   - BLE release task   (tears down NimBLE cleanly)
 *   - Alert dispatch task (sends HTTP alert via alert_dispatcher)
 *   - WebRTC stop task   (calls stop_webrtc)
 *   - CSI cooldown task  (delays then arms radar or CSI handler)
 */

#include "orchestrator_tasks.h"
#include "orchestrator_helpers.h"

#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_common.h"
#include "ble_device_control.h"
#include "ble_device_callbacks.h"
#include "alert_dispatcher.h"
#include "webrtc.h"
#include "csi_handler.h"
// Removed radar.h and sensor_dock.h
#include "common.h"
#include "ui.h"     /* COLOR_*_BGR565 constants */

static const char *TAG = "MAIN";

/* ── BLE Prepare Task ───────────────────────────────────────────────────── */

static TaskHandle_t s_ble_prepare_task_handle = NULL;

/**
 * @brief Task body: cold-boot BLE host and prepare GAP scanner.
 *
 * Any failure causes an immediate ORCH_EVENT_BLE_BUSY post so the
 * orchestrator can fall back gracefully.
 */
static void orchestrator_ble_prepare_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Cold-booting BLE stack for identity validation");
    orchestrator_log_heap_snapshot("ble_prepare:before");

    esp_err_t err = ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE pre-clean release failed: %s", esp_err_to_name(err));
        orchestrator_log_heap_snapshot("ble_prepare:preclean_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_common_ensure_ready(ble_sync_semaphore, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE host cold-boot failed: %s", esp_err_to_name(err));
        ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        orchestrator_log_heap_snapshot("ble_prepare:host_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_device_control_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE central cold-boot failed: %s", esp_err_to_name(err));
        ble_device_full_release(BLE_RELEASE_TIMEOUT_MS);
        orchestrator_log_heap_snapshot("ble_prepare:central_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        s_ble_prepare_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = ble_device_prepare_for_identity_scan(BLE_RELEASE_TIMEOUT_MS);
    if (err != ESP_OK) {
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

void orchestrator_start_ble_prepare(void)
{
    if (s_ble_prepare_task_handle != NULL) {
        ESP_LOGW(TAG, "BLE prepare already running");
        return;
    }

    BaseType_t rc = orchestrator_create_external_stack_task(orchestrator_ble_prepare_task,
                                                            "ble_prepare",
                                                            6144,
                                                            NULL,
                                                            6,
                                                            &s_ble_prepare_task_handle);
    if (rc != pdPASS) {
        s_ble_prepare_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BLE prepare task");
        orchestrator_log_heap_snapshot("ble_prepare:create_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
    }
}

/* ── Identity Validation ────────────────────────────────────────────────── */

/**
 * @brief Start bounded BLE identity validation after BLE has been prepared.
 *
 * BLE host initialization and central startup are handled by STATE_PREPARING_BLE.
 * Any failure here is a stack/lifecycle fault and must not be interpreted as
 * an unauthorized identity rejection.
 */
void orchestrator_start_identity_validation(void)
{
    ESP_LOGI(TAG, "Starting bounded BLE identity validation (%d ms)",
             IDENTITY_VALIDATION_TIMEOUT_MS);
    orchestrator_log_heap_snapshot("identity_validation:start");

    esp_err_t err = ble_device_start_identity_validation(IDENTITY_VALIDATION_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE identity validation could not be scheduled: %s",
                 esp_err_to_name(err));
        orchestrator_log_heap_snapshot("identity_validation:schedule_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_BUSY);
        return;
    }

    orchestrator_log_heap_snapshot("identity_validation:scheduled");
}

/* ── BLE Release Task ───────────────────────────────────────────────────── */

static TaskHandle_t s_ble_release_task_handle = NULL;

/**
 * @brief Task body: release all BLE resources.
 *
 * Logs heap state before and after, then posts the appropriate result event.
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
 * @brief Start the BLE release process in a dedicated task.
 *
 * Creates a dedicated task to handle the release asynchronously, allowing
 * the orchestrator to continue processing events.  Guards against concurrent
 * release operations.
 */
void orchestrator_start_ble_release(void)
{
    if (s_ble_release_task_handle != NULL) {
        ESP_LOGW(TAG, "BLE release already running");
        return;
    }

    BaseType_t rc = orchestrator_create_external_stack_task(orchestrator_ble_release_task,
                                                            "ble_release",
                                                            4096,
                                                            NULL,
                                                            6,
                                                            &s_ble_release_task_handle);
    if (rc != pdPASS) {
        s_ble_release_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BLE release task");
        orchestrator_log_heap_snapshot("ble_release:create_failed");
        orchestrator_post_event(ORCH_EVENT_BLE_RELEASE_FAILED);
    }
}

/* ── Alert Dispatch Task ────────────────────────────────────────────────── */

typedef struct
{
    uint32_t timestamp_ms;
    float    corr_drop;
    bool     reinforcement;
} alert_dispatch_task_ctx_t;

/**
 * @brief Task body: send the alert via alert_dispatcher_send_alert().
 *
 * For primary alerts, posts the result event to the orchestrator queue.
 * For reinforcement alerts, only logs the outcome.
 */
static void orchestrator_alert_dispatch_task(void *param)
{
    alert_dispatch_task_ctx_t *ctx = (alert_dispatch_task_ctx_t *)param;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = alert_dispatcher_send_alert(ctx->timestamp_ms, ctx->corr_drop);
    if (ctx->reinforcement)
    {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Vigilante reinforcement alert dispatched");
        } else {
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

esp_err_t orchestrator_start_alert_dispatch(uint32_t timestamp_ms,
                                            float    corr_drop,
                                            bool     reinforcement)
{
    TaskHandle_t *task_handle = reinforcement
                                    ? &s_vigilante_reinforcement_task_handle
                                    : &s_alert_dispatch_task_handle;
    if (*task_handle != NULL) {
        ESP_LOGW(TAG, "%s alert dispatch already running",
                 reinforcement ? "Reinforcement" : "Initial");
        return ESP_ERR_INVALID_STATE;
    }

    alert_dispatch_task_ctx_t *ctx = calloc(1, sizeof(alert_dispatch_task_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate alert dispatch context");
        return ESP_ERR_NO_MEM;
    }

    ctx->timestamp_ms = timestamp_ms;
    ctx->corr_drop    = corr_drop;
    ctx->reinforcement = reinforcement;

    BaseType_t rc = xTaskCreate(orchestrator_alert_dispatch_task,
                                reinforcement ? "alert_reinforce" : "alert_dispatch",
                                6144,
                                ctx,
                                5,
                                task_handle);
    if (rc != pdPASS) {
        *task_handle = NULL;
        free(ctx);
        ESP_LOGE(TAG, "Failed to create %s alert task",
                 reinforcement ? "reinforcement" : "initial");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ── WebRTC Stop Task ───────────────────────────────────────────────────── */

static TaskHandle_t s_webrtc_stop_task_handle = NULL;

/**
 * @brief Task body: call stop_webrtc() and post ORCH_EVENT_WEBRTC_STOPPED.
 */
static void orchestrator_webrtc_stop_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "STATE_STOPPING_WEBRTC: closing WebRTC stack");
    orchestrator_log_heap_snapshot("webrtc_stop:before");
    int ret = stop_webrtc();
    if (ret != 0) {
        ESP_LOGW(TAG, "WebRTC stop returned %d; continuing shutdown barrier", ret);
    }
    orchestrator_log_heap_snapshot("webrtc_stop:after");

    orchestrator_post_event(ORCH_EVENT_WEBRTC_STOPPED);
    s_webrtc_stop_task_handle = NULL;
    vTaskDelete(NULL);
}

void orchestrator_start_webrtc_stop(void)
{
    if (s_webrtc_stop_task_handle != NULL) {
        ESP_LOGW(TAG, "WebRTC stop already running");
        return;
    }

    s_webrtc_stop_started_ms = orchestrator_now_ms();
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(orchestrator_webrtc_stop_task,
                                                    "webrtc_stop",
                                                    WEBRTC_STOP_TASK_STACK_SIZE,
                                                    NULL,
                                                    WEBRTC_STOP_TASK_PRIORITY,
                                                    &s_webrtc_stop_task_handle,
                                                    tskNO_AFFINITY,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        s_webrtc_stop_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create WebRTC stop task");
        orchestrator_post_event(ORCH_EVENT_WEBRTC_STOPPED);
    }
}

bool orchestrator_webrtc_stop_timeout_expired(void)
{
    return s_webrtc_stop_started_ms != 0 &&
           (uint32_t)(orchestrator_now_ms() - s_webrtc_stop_started_ms) >= WEBRTC_STOP_TIMEOUT_MS;
}

/* ── CSI Cooldown Task ──────────────────────────────────────────────────── */

/**
 * @brief Task body: wait for the CSI quiet period, then arm the motion sensor.
 *
 * Uses a generation counter to detect and silently abort stale instances
 * (e.g., when the orchestrator transitions away from STATE_SLEEP before
 * the cooldown completes).
 */
static void orchestrator_sleep_csi_cooldown_task(void *param)
{
    uint32_t generation = (uint32_t)(uintptr_t)param;

    const uint32_t quiet_scan_delay_ms =
        (SLEEP_WIFI_READY_DISPLAY_MS < SLEEP_CSI_COOLDOWN_MS)
            ? SLEEP_WIFI_READY_DISPLAY_MS
            : SLEEP_CSI_COOLDOWN_MS;

    vTaskDelay(pdMS_TO_TICKS(quiet_scan_delay_ms));

    if (generation != s_sleep_csi_generation) {
        ESP_LOGD(TAG, "STATE_SLEEP: stale CSI cooldown task ignored");
        vTaskDelete(NULL);
        return;
    }

    orchestrator_show_phase("sleep_csi_cooldown", "Quiet scan", "Warming up", COLOR_CYAN_BGR565);

    if (SLEEP_CSI_COOLDOWN_MS > quiet_scan_delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(SLEEP_CSI_COOLDOWN_MS - quiet_scan_delay_ms));
    }

    if (generation == s_sleep_csi_generation)
    {
        ESP_LOGI(TAG, "STATE_SLEEP: Cooldown complete; starting unified motion sensing.");
        s_sleep_motion_allowed_ms = 0;

        esp_err_t csi_err = csi_handler_start();
        if (csi_err != ESP_OK) {
            ESP_LOGE(TAG, "STATE_SLEEP: failed to start CSI after cooldown: %s", esp_err_to_name(csi_err));
        } else {
            orchestrator_show_phase("csi_watch", "Watching", "Motion scan", COLOR_GREEN_BGR565);
        }
    }
    else
    {
        ESP_LOGD(TAG, "STATE_SLEEP: stale CSI cooldown task ignored");
    }

    vTaskDelete(NULL);
}

void orchestrator_schedule_sleep_csi_start(void)
{
    uint32_t generation       = ++s_sleep_csi_generation;
    s_sleep_motion_allowed_ms = orchestrator_now_ms() + SLEEP_CSI_COOLDOWN_MS;

    BaseType_t rc = xTaskCreate(orchestrator_sleep_csi_cooldown_task,
                                "csi_cooldown",
                                3072,
                                (void *)(uintptr_t)generation,
                                5,
                                NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "STATE_SLEEP: failed to create CSI cooldown task");
    }
}
