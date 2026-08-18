/**
 * @file audio_watchdog.c
 * @brief Watchdog de diagnóstico del pipeline de audio (AFE fetch).
 *
 * NO intrusivo: solo LEE estado (contadores de salud del audio, estados de
 * tareas vía uxTaskGetSystemState, heap). No toca memoria, prioridades ni
 * comportamiento del pipeline de audio/WebRTC.
 *
 * Detecta cuando el fetch() del AFE no avanza (> STALL_MS) y emite un reporte
 * con el estado de TODAS las tareas (nombre/prio/core/estado/stack), heap y
 * cola de audio, con rate-limit de REPORT_PERIOD_MS.
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_capture.h"
#include "media_sys.h"

#define AUD_WATCH_TAG   "AUD_WATCH"

#define WATCH_LOOP_MS          (100)
#define STALL_MS               (500)
#define REPORT_PERIOD_MS       (5000)
#define MAX_TASKS              (64)

typedef struct {
    TaskHandle_t        handle;
    bool                started;
    esp_capture_handle_t capture;
    TaskStatus_t       *task_status;
    uint32_t            task_status_cap;
    uint32_t            last_report_ms;
    bool                stall_active;
    uint32_t            stall_start_ms;
} audio_watchdog_ctx_t;

static audio_watchdog_ctx_t s_watchdog_ctx;

static const char *task_state_char(eTaskState state)
{
    switch (state) {
        case eRunning:   return "R";
        case eReady:     return "D";
        case eBlocked:   return "B";
        case eSuspended: return "S";
        case eDeleted:   return "X";
        default:         return "?";
    }
}

static void watchdog_dump_report(audio_watchdog_ctx_t *ctx,
                                 uint32_t last_fetch_ms,
                                 uint32_t fetch_count,
                                 uint32_t drop_frames,
                                 uint32_t q_fill,
                                 uint32_t q_cap)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age_ms = (now_ms >= last_fetch_ms) ? (now_ms - last_fetch_ms) : 0;

    ESP_LOGW(AUD_WATCH_TAG, "=== STALL de fetch() de audio detectado ===");
    ESP_LOGW(AUD_WATCH_TAG, "fetch age=%lu ms (desde %lu ms) | fetch_count=%lu | drop_frames=%lu | q_fill=%lu/%lu",
             (unsigned long)age_ms, (unsigned long)last_fetch_ms,
             (unsigned long)fetch_count, (unsigned long)drop_frames,
             (unsigned long)q_fill, (unsigned long)q_cap);

    ESP_LOGW(AUD_WATCH_TAG, "HEAP internal free=%zu min=%zu lrg=%zu | dma free=%zu lrg=%zu | psram free=%zu min=%zu lrg=%zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_DMA),
             heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (ctx->task_status == NULL || ctx->task_status_cap == 0) {
        return;
    }
    UBaseType_t num = uxTaskGetSystemState(ctx->task_status, ctx->task_status_cap, NULL);
    if (num == 0) {
        ESP_LOGW(AUD_WATCH_TAG, "uxTaskGetSystemState fallo (buffer insuficiente o RTOS no listo)");
        return;
    }
    ESP_LOGW(AUD_WATCH_TAG, "TASKS (%u):", (unsigned)num);
    for (UBaseType_t i = 0; i < num; i++) {
        TaskStatus_t *t = &ctx->task_status[i];
        ESP_LOGW(AUD_WATCH_TAG, "  %-16s prio=%2u core=%d state=%s stack_hw=%u",
                 t->pcTaskName, (unsigned)t->uxCurrentPriority, (int)t->xCoreID,
                 task_state_char(t->eCurrentState), (unsigned)t->usStackHighWaterMark);
    }
}

static void audio_watchdog_task(void *arg)
{
    audio_watchdog_ctx_t *ctx = (audio_watchdog_ctx_t *)arg;
    uint32_t last_fetch_ms = 0, fetch_count = 0, drop_frames = 0, q_fill = 0, q_cap = 0;
    bool fetching = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_LOOP_MS));

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (ctx->capture == NULL) {
            esp_webrtc_media_provider_t provider = { 0 };
            if (media_sys_get_provider(&provider) == 0 && provider.capture != NULL) {
                ctx->capture = provider.capture;
                ESP_LOGI(AUD_WATCH_TAG, "Vigilando capture %p (fetch health)", (void *)ctx->capture);
            } else {
                continue;
            }
        }

        if (esp_capture_get_audio_src_health(ctx->capture, &last_fetch_ms, &fetch_count,
                                             &drop_frames, &q_fill, &q_cap, &fetching) != ESP_CAPTURE_ERR_OK) {
            continue;
        }

        if (!fetching) {
            ctx->stall_active = false;
            continue;
        }

        /* Capture arrancando: aun no hubo la primera fetch() (last_fetch=0).
         * No es un stall: esperar a que el pipeline tome el pulso. */
        if (fetch_count == 0 && last_fetch_ms == 0) {
            ctx->stall_active = false;
            ctx->last_report_ms = now_ms;
            continue;
        }

        uint32_t age_ms = (now_ms >= last_fetch_ms) ? (now_ms - last_fetch_ms) : 0;

        if (age_ms > STALL_MS) {
            if (!ctx->stall_active) {
                ctx->stall_active = true;
                ctx->stall_start_ms = now_ms;
            }
            if (now_ms - ctx->last_report_ms >= REPORT_PERIOD_MS) {
                ctx->last_report_ms = now_ms;
                watchdog_dump_report(ctx, last_fetch_ms, fetch_count, drop_frames, q_fill, q_cap);
            }
        } else if (ctx->stall_active) {
            ctx->stall_active = false;
            ESP_LOGW(AUD_WATCH_TAG, "fetch() reanudado (stall duro %lu ms; fetch_count=%lu)",
                     (unsigned long)(now_ms - ctx->stall_start_ms), (unsigned long)fetch_count);
        }
    }
}

void audio_watchdog_start(void)
{
    audio_watchdog_ctx_t *ctx = &s_watchdog_ctx;
    if (ctx->started) {
        return;
    }
    ctx->task_status_cap = MAX_TASKS;
    ctx->task_status = heap_caps_malloc(MAX_TASKS * sizeof(TaskStatus_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->task_status == NULL) {
        ESP_LOGW(AUD_WATCH_TAG, "No se pudo asignar buffer de estados de tareas; reportes parciales");
    }
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(audio_watchdog_task, "aud_watch",
                                                    4096, ctx, 2, &ctx->handle, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
        ctx->handle = NULL;
        ESP_LOGE(AUD_WATCH_TAG, "No se pudo crear tarea watchdog");
        return;
    }
    ctx->started = true;
    ESP_LOGI(AUD_WATCH_TAG, "Watchdog de audio iniciado (stall >%d ms, reporte cada %d ms)",
             STALL_MS, REPORT_PERIOD_MS);
}