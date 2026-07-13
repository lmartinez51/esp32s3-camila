/**
 * @file simi.c
 * @brief Dr. Simi procedural mascot — parametric face renderer (Phase 1).
 *
 *        Builds a stylized "chibi" Dr. Simi (white hair + bushy mustache, round
 *        glasses, lab coat, red bowtie) entirely from rasterizer primitives, into a
 *        single off-screen canvas that is blitted to the panel in one transaction.
 *
 *        The face is fully parameterized by simi_face_t, so Phase 2/3 animation only
 *        needs to vary those fields over time and re-render.
 *
 * @author Lorenzo Martínez
 * @platform ESP32-S3-BOX3
 */
#include "simi.h"
#include "simi_canvas.h"
#include "ui.h"

#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>
#include <math.h>

static const char *TAG = "SIMI";

/* ── Geometría del lienzo (pantalla completa 320×240) ── */
#define SIMI_CANVAS_W 320
#define SIMI_CANVAS_H 240
#define SIMI_BLIT_X 0
#define SIMI_BLIT_Y 0
#define SIMI_CX 160

#define SIMI_DIRTY_BUF_SIZE 8192
static uint16_t *s_dirty_buf = NULL;

#define SIMI_ANIM_STACK_SIZE 4096
#define SIMI_ANIM_PRIORITY (tskIDLE_PRIORITY + 1)
#define SIMI_ANIM_TRY_LOCK_MS 5
#define SIMI_ANIM_STOP_WAIT_MS 2600
#define SIMI_IDLE_WAKE_MS 500
#define SIMI_IDLE_BLINK_MIN_MS 5200
#define SIMI_IDLE_BLINK_JITTER_MS 2200

/* ── Paleta (RGB lógico; SIMI_RGB la traduce al formato del panel) ── */
#define C_BG SIMI_RGB(0, 0, 0)
#define C_SKIN SIMI_RGB(245, 205, 170)
#define C_SKIN_SH SIMI_RGB(220, 175, 140)
#define C_HAIR SIMI_RGB(248, 248, 248)
#define C_COAT SIMI_RGB(252, 252, 252)
#define C_COAT_SH SIMI_RGB(218, 218, 224)
#define C_SHIRT SIMI_RGB(200, 220, 245)
#define C_TIE SIMI_RGB(15, 60, 145)
#define C_HAIR_BLACK SIMI_RGB(30, 30, 35)
#define C_EYE SIMI_RGB(40, 35, 45)
#define C_EYE_HI SIMI_RGB(255, 255, 255)
#define C_MOUTH SIMI_RGB(150, 55, 55)
#define C_MOUTH_OPEN SIMI_RGB(105, 30, 38)
#define C_TONGUE SIMI_RGB(230, 90, 90)
#define C_BLUSH SIMI_RGB(240, 150, 150)
#define C_CROSS SIMI_RGB(210, 35, 45)
#define C_ALERT SIMI_RGB(235, 30, 30)
#define C_ALERT_BG SIMI_RGB(22, 0, 0)

#define C_JERSEY_GREEN SIMI_RGB(20, 140, 60)
#define C_JERSEY_SH SIMI_RGB(15, 110, 45)
#define C_JERSEY_RED SIMI_RGB(200, 30, 40)
#define C_CHAPULIN_RED SIMI_RGB(220, 40, 40)
#define C_CHAPULIN_SH SIMI_RGB(180, 20, 20)
#define C_CHAPULIN_YELLOW SIMI_RGB(240, 210, 40)

static simi_canvas_t s_cv = {0};
static uint16_t *s_static_bg_buf = NULL; // T1: Static Cache Buffer
static volatile bool s_bg_rendered = false;       // T1: One-Time Render flag
static bool s_simi_screen_cleared = false;
static bool s_simi_backlight_ready = false;

static char s_simi_overlay_text[32] = {0};
static uint16_t s_simi_overlay_color = 0;

static char s_simi_top_right_text[32] = {0};

static char s_arbiter_slot1[32] = {0};
static char s_arbiter_slot2[32] = {0};
static char s_arbiter_slot3[32] = "Lorenzo";

static SemaphoreHandle_t s_anim_mutex = NULL;
static TaskHandle_t s_anim_task = NULL;
static bool s_anim_stop_requested = false;
static simi_state_t s_anim_state = SIMI_STATE_IDLE;
static volatile bool s_anim_speaking = false;
static volatile simi_outfit_t s_active_outfit = OUTFIT_DOCTOR_WHITE;

static void simi_render(const simi_face_t *f);

static esp_err_t simi_anim_ensure_mutex(void)
{
    if (s_anim_mutex == NULL)
    {
        s_anim_mutex = xSemaphoreCreateMutex();
        if (s_anim_mutex == NULL)
        {
            ESP_LOGE(TAG, "No se pudo crear el mutex de animacion de Dr. Simi");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static uint32_t simi_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t simi_next_blink_delay_ms(uint32_t now_ms)
{
    return SIMI_IDLE_BLINK_MIN_MS + (now_ms % SIMI_IDLE_BLINK_JITTER_MS);
}

static simi_state_t simi_effective_state(simi_state_t state, bool speaking)
{
    if (speaking &&
        state != SIMI_STATE_MUTED &&
        state != SIMI_STATE_ALERT &&
        state != SIMI_STATE_SAD &&
        state != SIMI_STATE_SLEEP)
    {
        return SIMI_STATE_TALKING;
    }
    return state;
}

static void simi_anim_snapshot(simi_state_t *state, bool *speaking, bool *stop_requested)
{
    simi_state_t local_state = SIMI_STATE_IDLE;
    bool local_speaking = s_anim_speaking;
    bool local_stop = (s_anim_mutex == NULL);

    if (s_anim_mutex != NULL &&
        xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        local_state = s_anim_state;
        local_stop = s_anim_stop_requested;
        xSemaphoreGive(s_anim_mutex);
    }

    if (state)
    {
        *state = local_state;
    }
    if (speaking)
    {
        *speaking = local_speaking;
    }
    if (stop_requested)
    {
        *stop_requested = local_stop;
    }
}

static bool simi_anim_is_running(void)
{
    bool running = (s_anim_mutex != NULL);
    if (s_anim_mutex != NULL &&
        xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        running = (s_anim_task != NULL);
        xSemaphoreGive(s_anim_mutex);
    }
    return running;
}

static void simi_apply_animation(simi_state_t visual_state,
                                 uint32_t frame,
                                 bool speaking,
                                 bool blink_active,
                                 uint8_t blink_frame,
                                 simi_face_t *f)
{
    if (!f)
    {
        return;
    }

    switch (visual_state)
    {
    case SIMI_STATE_TALKING:
    {
        static const int16_t mouth_frames[] = {35, 70, 45, 85};
        f->mouth_open = mouth_frames[frame % (sizeof(mouth_frames) / sizeof(mouth_frames[0]))];
        f->head_dy = (frame & 1) ? -1 : 1;
        break;
    }
    case SIMI_STATE_THINKING:
        f->head_dy = (int16_t)((frame % 4 == 1) ? -2 : ((frame % 4 == 3) ? 1 : 0));
        f->eyes_up = true;
        break;
    case SIMI_STATE_ALERT:
    {
        static const int16_t alert_mouth_frames[] = {18, 46, 24, 56};
        // Option A: Stealth Alert. Do NOT toggle alert_border to preserve cache.
        f->head_dy = (frame & 1) ? -1 : 0;
        if (speaking)
        {
            f->mouth_open = alert_mouth_frames[frame % (sizeof(alert_mouth_frames) / sizeof(alert_mouth_frames[0]))];
        }
        break;
    }
    case SIMI_STATE_LISTENING:
    case SIMI_STATE_IDLE:
    case SIMI_STATE_HAPPY:
        if (blink_active)
        {
            static const int16_t blink_eye_frames[] = {100, 35, 0, 35, 100};
            uint8_t idx = blink_frame;
            if (idx >= sizeof(blink_eye_frames) / sizeof(blink_eye_frames[0]))
            {
                idx = (uint8_t)((sizeof(blink_eye_frames) / sizeof(blink_eye_frames[0])) - 1);
            }
            f->eye_open = blink_eye_frames[idx];
        }
        break;
    case SIMI_STATE_SLEEP:
    case SIMI_STATE_MUTED:
    {
        // Lightweight triangle wave over 16 frames for slow breathing
        uint32_t phase = frame % 16;
        int breath_val = (phase < 8) ? phase : (15 - phase); // 0 up to 7, then back to 0
        
        f->head_dy += (breath_val / 3);    // Gentle head bob (0 to 2 pixels)
        f->mouth_open += (breath_val / 2); // Subtle mouth expansion (0 to 3 pixels)
        f->bubble_radius = breath_val * 2; // Snot bubble inflates/deflates (0 to 14 pixels)
        break;
    }
    default:
        break;
    }
}

static bool simi_blit_dirty_rect(int x0, int y0, int x1, int y1, bool blocking)
{
    if (!s_dirty_buf) return false;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_cv.w - 1) x1 = s_cv.w - 1;
    if (y1 > s_cv.h - 1) y1 = s_cv.h - 1;
    if (x0 > x1 || y0 > y1) return true;

    if (!s_simi_screen_cleared)
    {
        ui_clear_screen();
        s_simi_screen_cleared = true;
        blocking = true;
    }

    int w = x1 - x0 + 1;
    int max_rows = SIMI_DIRTY_BUF_SIZE / (w * sizeof(uint16_t));
    if (max_rows < 1) max_rows = 1;

    bool ok = true;
    for (int y = y0; y <= y1; y += max_rows)
    {
        int rows = y1 - y + 1 < max_rows ? y1 - y + 1 : max_rows;
        for (int r = 0; r < rows; r++)
        {
            memcpy(&s_dirty_buf[r * w], &s_cv.buf[(y + r) * s_cv.w + x0], w * sizeof(uint16_t));
        }
        if (blocking)
        {
            ui_panel_blit(SIMI_BLIT_X + x0, SIMI_BLIT_Y + y,
                          SIMI_BLIT_X + x0 + w, SIMI_BLIT_Y + y + rows,
                          s_dirty_buf);
        }
        else
        {
            ok = ui_panel_try_blit(SIMI_BLIT_X + x0, SIMI_BLIT_Y + y,
                                   SIMI_BLIT_X + x0 + w, SIMI_BLIT_Y + y + rows,
                                   s_dirty_buf, SIMI_ANIM_TRY_LOCK_MS);
        }
        if (!ok) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (ok && !s_simi_backlight_ready)
    {
        ui_backlight_on();
        s_simi_backlight_ready = true;
    }
    return ok;
}

static bool simi_blit_frame(bool blocking)
{
    return simi_blit_dirty_rect(0, 0, s_cv.w - 1, s_cv.h - 1, blocking);
}

static void simi_anim_task(void *arg)
{
    (void)arg;

    simi_state_t last_visual_state = SIMI_STATE_MAX;
    bool last_speaking = false;
    uint32_t frame = 0;
    uint32_t wait_ms = 0;
    uint32_t next_blink_ms = simi_now_ms() + 1200;
    bool blink_active = false;
    uint8_t blink_frame = 0;

    uint32_t last_render_ms = 0;

    while (1)
    {
        if (wait_ms > 0)
        {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        }
        else
        {
            (void)ulTaskNotifyTake(pdTRUE, 0);
        }

        uint32_t current_ms = simi_now_ms();
        if (last_render_ms != 0 && (current_ms - last_render_ms) < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(100 - (current_ms - last_render_ms)));
        }

        simi_state_t base_state = SIMI_STATE_IDLE;
        bool speaking = false;
        bool stop_requested = true;
        simi_anim_snapshot(&base_state, &speaking, &stop_requested);

        if (stop_requested || !ui_simi_ready() || !ui_is_initialized())
        {
            break;
        }

        uint32_t now_ms = simi_now_ms();
        simi_state_t visual_state = simi_effective_state(base_state, speaking);
        bool state_changed = (visual_state != last_visual_state) || (speaking != last_speaking);
        bool render = state_changed;
        bool blocking_blit = true;

        static simi_face_t current_f = {0};
        static bool current_f_init = false;
        simi_face_t target_f;
        simi_face_for_state(visual_state, &target_f);
        
        if (!current_f_init) {
            current_f = target_f;
            current_f_init = true;
        }

        bool needs_interp = (current_f.eye_open != target_f.eye_open) ||
                            (current_f.mouth_curve != target_f.mouth_curve) ||
                            (current_f.mouth_open != target_f.mouth_open);

        wait_ms = SIMI_IDLE_WAKE_MS;

        if (visual_state == SIMI_STATE_TALKING)
        {
            render = true;
            wait_ms = 250;
        }
        else if (visual_state == SIMI_STATE_THINKING)
        {
            render = true;
            wait_ms = 500;
        }
        else if (visual_state == SIMI_STATE_ALERT)
        {
            render = true;
            wait_ms = 333;
        }
        else if (visual_state == SIMI_STATE_SLEEP || visual_state == SIMI_STATE_MUTED)
        {
            render = true;
            wait_ms = 150;
        }
        else if (visual_state == SIMI_STATE_LISTENING ||
                 visual_state == SIMI_STATE_IDLE ||
                 visual_state == SIMI_STATE_HAPPY)
        {
            if (!blink_active && now_ms >= next_blink_ms)
            {
                blink_active = true;
                blink_frame = 0;
            }

            if (blink_active)
            {
                render = true;
                wait_ms = 140;
            }
            else if (next_blink_ms > now_ms)
            {
                uint32_t until_blink_ms = next_blink_ms - now_ms;
                wait_ms = until_blink_ms < SIMI_IDLE_WAKE_MS ? until_blink_ms : SIMI_IDLE_WAKE_MS;
            }
        }

        if (needs_interp) {
            render = true;
            wait_ms = 50; // Fast loop during interpolation
        }

        if (render)
        {
            last_render_ms = simi_now_ms();
            
            int step = 25;
            if (current_f.eye_open < target_f.eye_open) {
                current_f.eye_open += step;
                if (current_f.eye_open > target_f.eye_open) current_f.eye_open = target_f.eye_open;
            } else if (current_f.eye_open > target_f.eye_open) {
                current_f.eye_open -= step;
                if (current_f.eye_open < target_f.eye_open) current_f.eye_open = target_f.eye_open;
            }
            
            if (current_f.mouth_curve < target_f.mouth_curve) {
                current_f.mouth_curve += step;
                if (current_f.mouth_curve > target_f.mouth_curve) current_f.mouth_curve = target_f.mouth_curve;
            } else if (current_f.mouth_curve > target_f.mouth_curve) {
                current_f.mouth_curve -= step;
                if (current_f.mouth_curve < target_f.mouth_curve) current_f.mouth_curve = target_f.mouth_curve;
            }

            if (current_f.mouth_open < target_f.mouth_open) {
                current_f.mouth_open += step;
                if (current_f.mouth_open > target_f.mouth_open) current_f.mouth_open = target_f.mouth_open;
            } else if (current_f.mouth_open > target_f.mouth_open) {
                current_f.mouth_open -= step;
                if (current_f.mouth_open < target_f.mouth_open) current_f.mouth_open = target_f.mouth_open;
            }

            current_f.brow_angle = target_f.brow_angle;
            current_f.eyes_up = target_f.eyes_up;
            current_f.alert_border = target_f.alert_border;
            current_f.bg = target_f.bg;
            
            if (visual_state != SIMI_STATE_SLEEP && visual_state != SIMI_STATE_MUTED) {
                current_f.bubble_radius = 0; // Instantly reset when awake
            }
            
            simi_face_t f = current_f;
            simi_apply_animation(visual_state, frame, speaking, blink_active, blink_frame, &f);

            static simi_face_t last_f = {0};
            static char last_overlay_text[32] = {0};
            static char last_top_right_text[32] = {0};
            static simi_outfit_t last_outfit = OUTFIT_MAX;
            
            bool text_changed = false;
            bool top_right_changed = false;
            bool outfit_changed = false;

            if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                text_changed = strncmp(s_simi_overlay_text, last_overlay_text, sizeof(last_overlay_text)) != 0;
                if (text_changed) strncpy(last_overlay_text, s_simi_overlay_text, sizeof(last_overlay_text));
                
                top_right_changed = strncmp(s_simi_top_right_text, last_top_right_text, sizeof(last_top_right_text)) != 0;
                if (top_right_changed) strncpy(last_top_right_text, s_simi_top_right_text, sizeof(last_top_right_text));

                outfit_changed = (s_active_outfit != last_outfit);
                if (outfit_changed) last_outfit = s_active_outfit;
                
                xSemaphoreGive(s_anim_mutex);
            }

            bool full_redraw = (visual_state != last_visual_state) || 
                               (last_visual_state == SIMI_STATE_MAX) || 
                               outfit_changed || 
                               text_changed || 
                               top_right_changed || 
                               f.alert_border;

            if (full_redraw)
            {
                s_cv.clip_x0 = 0; s_cv.clip_y0 = 0; s_cv.clip_x1 = s_cv.w - 1; s_cv.clip_y1 = s_cv.h - 1;
                simi_render(&f);
                simi_blit_dirty_rect(0, 0, 319, 239, blocking_blit);
            }
            else
            {
                // Normal speech and blinking animation uses the strict facial dirty rect
                s_cv.clip_x0 = 60; s_cv.clip_y0 = 30; s_cv.clip_x1 = 260; s_cv.clip_y1 = 170;
                simi_render(&f);
                simi_blit_dirty_rect(60, 30, 260, 170, blocking_blit);
            }

            last_f = f;
            last_visual_state = visual_state;
            last_speaking = speaking;

            frame++;

            if (blink_active)
            {
                blink_frame++;
                if (blink_frame >= 5)
                {
                    blink_active = false;
                    next_blink_ms = now_ms + simi_next_blink_delay_ms(now_ms);
                }
            }
        }

        // Strict framerate cap to unblock PSRAM SPI bus for WebRTC/JSON tasks
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_anim_mutex != NULL &&
        xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        s_anim_task = NULL;
        s_anim_stop_requested = false;
        s_anim_speaking = false;
        xSemaphoreGive(s_anim_mutex);
    }

    vTaskDelete(NULL);
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Canvas lifecycle                                                          */
/* ──────────────────────────────────────────────────────────────────────── */

esp_err_t ui_simi_init(void)
{
    if (s_cv.buf)
    {
        return ESP_OK; // ya inicializado
    }

    const size_t bytes = (size_t)SIMI_CANVAS_W * SIMI_CANVAS_H * sizeof(uint16_t);

    // Diagnóstico: estado de la RAM interna ANTES de reservar el lienzo.
    // ESP_LOGW(TAG, "[HEAP] simi_canvas:before | INTERNAL free=%u largest=%u | PSRAM free=%u",
    //          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    //          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    //          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // El canvas DEBE vivir en PSRAM: la RAM interna es escasa y la necesita el
    // controlador BLE (su init falla con ESP_ERR_NO_MEM si se la quitamos).
    // OJO: NO usar MALLOC_CAP_DMA — la PSRAM no está marcada como DMA-capable, así
    // que SPIRAM|DMA es insatisfacible y caería a RAM interna. El blit por SPI desde
    // PSRAM funciona en el S3 (igual que los buffers malloc del texto existente).
    s_cv.buf = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    s_static_bg_buf = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM); // T1: Allocate static layer buffer
    
    if (s_cv.buf && s_static_bg_buf)
    {
        ESP_LOGI(TAG, "Canvas y Static BG %dx%d (%u B c/u) en PSRAM", SIMI_CANVAS_W, SIMI_CANVAS_H, (unsigned)bytes);
    }
    else
    {
        if (s_cv.buf) { heap_caps_free(s_cv.buf); s_cv.buf = NULL; }
        if (s_static_bg_buf) { heap_caps_free(s_static_bg_buf); s_static_bg_buf = NULL; }
    }

    if (!s_cv.buf || !s_static_bg_buf)
    {
        ESP_LOGE(TAG, "Fallo al asignar buffers de Dr. Simi en PSRAM (%u B)", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }

    if (!s_dirty_buf) {
        s_dirty_buf = heap_caps_malloc(SIMI_DIRTY_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dirty_buf) {
            ESP_LOGE(TAG, "Fallo al asignar s_dirty_buf en DMA (%u B)", SIMI_DIRTY_BUF_SIZE);
            heap_caps_free(s_cv.buf);
            s_cv.buf = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_cv.w = SIMI_CANVAS_W;
    s_cv.h = SIMI_CANVAS_H;
    s_cv.clip_x0 = 0; s_cv.clip_y0 = 0; s_cv.clip_x1 = s_cv.w - 1; s_cv.clip_y1 = s_cv.h - 1;
    s_simi_screen_cleared = false;
    s_simi_backlight_ready = false;

    // ESP_LOGW(TAG, "[HEAP] simi_canvas:after  | ptr=%p ext_ram=%d | INTERNAL free=%u largest=%u | PSRAM free=%u",
    //          s_cv.buf, esp_ptr_external_ram(s_cv.buf) ? 1 : 0,
    //          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    //          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    //          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

bool ui_simi_ready(void)
{
    return s_cv.buf != NULL;
}

esp_err_t ui_simi_start(void)
{
    if (!ui_simi_ready() || !ui_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = simi_anim_ensure_mutex();
    if (err != ESP_OK)
    {
        return err;
    }

    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_anim_task != NULL)
    {
        s_anim_stop_requested = false;
        TaskHandle_t task = s_anim_task;
        xSemaphoreGive(s_anim_mutex);
        xTaskNotifyGive(task);
        return ESP_OK;
    }

    s_anim_stop_requested = false;
    xSemaphoreGive(s_anim_mutex);

    TaskHandle_t new_task = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(simi_anim_task,
                                            "simi_anim",
                                            SIMI_ANIM_STACK_SIZE,
                                            NULL,
                                            SIMI_ANIM_PRIORITY,
                                            &new_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS || new_task == NULL)
    {
        ESP_LOGE(TAG, "No se pudo crear simi_anim_task");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        s_anim_task = new_task;
        xSemaphoreGive(s_anim_mutex);
    }

    xTaskNotifyGive(new_task);
    
    // Inject the initial trigger for the arbiter slot 3 now that the task is ready
    ui_simi_set_arbiter_slot(3, "Lorenzo");
    
    return ESP_OK;
}

void ui_simi_stop(void)
{
    TaskHandle_t task = NULL;

    if (s_anim_mutex != NULL &&
        xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        task = s_anim_task;
        if (task != NULL)
        {
            s_anim_stop_requested = true;
            xTaskNotifyGive(task);
        }
        xSemaphoreGive(s_anim_mutex);
    }

    if (task != NULL && task != xTaskGetCurrentTaskHandle())
    {
        uint32_t waited_ms = 0;
        while (waited_ms < SIMI_ANIM_STOP_WAIT_MS)
        {
            TaskHandle_t current = NULL;
            if (s_anim_mutex != NULL &&
                xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                current = s_anim_task;
                xSemaphoreGive(s_anim_mutex);
            }

            if (current == NULL)
            {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }

        if (s_anim_mutex != NULL &&
            xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (s_anim_task != NULL)
            {
                ESP_LOGW(TAG, "simi_anim_task no salio dentro de %u ms", (unsigned)SIMI_ANIM_STOP_WAIT_MS);
            }
            xSemaphoreGive(s_anim_mutex);
        }
    }
}

void ui_simi_set_state(simi_state_t state)
{
    if (state < 0 || state >= SIMI_STATE_MAX)
    {
        return;
    }

    if (simi_anim_ensure_mutex() != ESP_OK)
    {
        return;
    }

    TaskHandle_t task = NULL;
    bool changed = false;
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        changed = (s_anim_state != state);
        s_anim_state = state;
        if (state == SIMI_STATE_TALKING)
        {
            s_anim_speaking = true;
        }
        else
        {
            s_anim_speaking = false;
        }
        task = s_anim_task;
        xSemaphoreGive(s_anim_mutex);
    }

    if (changed && task != NULL)
    {
        xTaskNotifyGive(task);
    }
}

void ui_simi_set_outfit(simi_outfit_t outfit)
{
    if (outfit >= OUTFIT_MAX)
    {
        return;
    }

    if (s_active_outfit != outfit)
    {
        s_active_outfit = outfit;
        s_bg_rendered = false;
        if (s_anim_task != NULL)
        {
            xTaskNotifyGive(s_anim_task);
        }
    }
}

void ui_simi_notify_speaking(bool active)
{
    s_anim_speaking = active;
}

void ui_simi_set_overlay_text(const char *text, uint16_t color)
{
    if (simi_anim_ensure_mutex() != ESP_OK)
    {
        return;
    }

    TaskHandle_t task = NULL;
    bool changed = false;
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        if (text)
        {
            if (strncmp(s_simi_overlay_text, text, sizeof(s_simi_overlay_text)) != 0 || s_simi_overlay_color != color)
            {
                strncpy(s_simi_overlay_text, text, sizeof(s_simi_overlay_text) - 1);
                s_simi_overlay_text[sizeof(s_simi_overlay_text) - 1] = '\0';
                s_simi_overlay_color = color;
                changed = true;
            }
        }
        else
        {
            if (s_simi_overlay_text[0] != '\0')
            {
                s_simi_overlay_text[0] = '\0';
                changed = true;
            }
        }
        if (changed)
        {
            task = s_anim_task;
        }
        xSemaphoreGive(s_anim_mutex);
    }

    if (task != NULL && changed)
    {
        xTaskNotifyGive(task);
    }
}

void ui_simi_set_arbiter_slot(int slot, const char *text)
{
    if (simi_anim_ensure_mutex() != ESP_OK)
    {
        return;
    }

    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        char *target_slot = NULL;
        if (slot == 1) target_slot = s_arbiter_slot1;
        else if (slot == 2) target_slot = s_arbiter_slot2;
        else if (slot == 3) target_slot = s_arbiter_slot3;

        if (target_slot != NULL)
        {
            if (text)
            {
                strncpy(target_slot, text, 31);
                target_slot[31] = '\0';
            }
            else
            {
                target_slot[0] = '\0';
            }
        }

        const char *selected_text = NULL;
        if (s_arbiter_slot1[0] != '\0') selected_text = s_arbiter_slot1;
        else if (s_arbiter_slot2[0] != '\0') selected_text = s_arbiter_slot2;
        else selected_text = s_arbiter_slot3;

        xSemaphoreGive(s_anim_mutex);
        
        ui_simi_set_top_right_text(selected_text);
    }
}

void ui_simi_set_top_right_text(const char *text)
{
    if (simi_anim_ensure_mutex() != ESP_OK)
    {
        return;
    }

    TaskHandle_t task = NULL;
    bool changed = false;
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        if (text)
        {
            if (strncmp(s_simi_top_right_text, text, sizeof(s_simi_top_right_text)) != 0)
            {
                strncpy(s_simi_top_right_text, text, sizeof(s_simi_top_right_text) - 1);
                s_simi_top_right_text[sizeof(s_simi_top_right_text) - 1] = '\0';
                changed = true;
            }
        }
        else
        {
            if (s_simi_top_right_text[0] != '\0')
            {
                s_simi_top_right_text[0] = '\0';
                changed = true;
            }
        }
        if (changed)
        {
            task = s_anim_task;
        }
        xSemaphoreGive(s_anim_mutex);
    }

    if (task != NULL && changed)
    {
        xTaskNotifyGive(task);
    }
}

void ui_simi_try_set_top_right_text(const char *text)
{
    if (simi_anim_ensure_mutex() != ESP_OK) return;

    TaskHandle_t task = NULL;
    bool changed = false;
    if (xSemaphoreTake(s_anim_mutex, 0) == pdTRUE)
    {
        if (text)
        {
            if (strncmp(s_simi_top_right_text, text, sizeof(s_simi_top_right_text)) != 0)
            {
                strncpy(s_simi_top_right_text, text, sizeof(s_simi_top_right_text) - 1);
                s_simi_top_right_text[sizeof(s_simi_top_right_text) - 1] = '\0';
                changed = true;
            }
        }
        else
        {
            if (s_simi_top_right_text[0] != '\0')
            {
                s_simi_top_right_text[0] = '\0';
                changed = true;
            }
        }
        if (changed)
        {
            task = s_anim_task;
        }
        xSemaphoreGive(s_anim_mutex);
    }

    if (task != NULL && changed)
    {
        xTaskNotifyGive(task);
    }
}

void ui_simi_try_set_arbiter_slot(int slot, const char *text)
{
    if (simi_anim_ensure_mutex() != ESP_OK) return;

    if (xSemaphoreTake(s_anim_mutex, 0) == pdTRUE)
    {
        char *target_slot = NULL;
        if (slot == 1) target_slot = s_arbiter_slot1;
        else if (slot == 2) target_slot = s_arbiter_slot2;
        else if (slot == 3) target_slot = s_arbiter_slot3;

        if (target_slot != NULL)
        {
            if (text)
            {
                strncpy(target_slot, text, 31);
                target_slot[31] = '\0';
            }
            else
            {
                target_slot[0] = '\0';
            }
        }

        const char *selected_text = NULL;
        if (s_arbiter_slot1[0] != '\0') selected_text = s_arbiter_slot1;
        else if (s_arbiter_slot2[0] != '\0') selected_text = s_arbiter_slot2;
        else selected_text = s_arbiter_slot3;

        xSemaphoreGive(s_anim_mutex);
        
        ui_simi_try_set_top_right_text(selected_text);
    }
}

void ui_simi_deinit(void)
{
    ui_simi_stop();

    if (simi_anim_is_running())
    {
        uint32_t extra_wait_ms = 0;
        const uint32_t MAX_EXTRA_WAIT = 1000;
        while (simi_anim_is_running() && extra_wait_ms < MAX_EXTRA_WAIT) {
            vTaskDelay(pdMS_TO_TICKS(50));
            extra_wait_ms += 50;
        }

        if (simi_anim_is_running()) {
            ESP_LOGE(TAG, "CRITICAL ERROR: simi_anim_task refuses to terminate! Skipping PSRAM free to prevent Use-After-Free.");
            return;
        }
    }

    if (s_cv.buf)
    {
        heap_caps_free(s_cv.buf);
        s_cv.buf = NULL;
    }
    if (s_static_bg_buf)
    {
        heap_caps_free(s_static_bg_buf);
        s_static_bg_buf = NULL;
    }
    if (s_dirty_buf)
    {
        heap_caps_free(s_dirty_buf);
        s_dirty_buf = NULL;
    }
    s_bg_rendered = false;
    s_simi_screen_cleared = false;
    s_simi_backlight_ready = false;
    s_cv.w = s_cv.h = 0;
    
    // Reset static states to ensure clean slate on next initialization
    s_simi_overlay_text[0] = '\0';
    s_simi_top_right_text[0] = '\0';
    s_anim_state = SIMI_STATE_IDLE;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  State → face parameters                                                   */
/* ──────────────────────────────────────────────────────────────────────── */

void simi_face_for_state(simi_state_t state, simi_face_t *f)
{
    if (!f)
    {
        return;
    }
    // Valores neutros por defecto (cara despierta y amable)
    f->eye_open = 100;
    f->mouth_curve = 40;
    f->mouth_open = 0;
    f->brow_angle = 0;
    f->head_dy = 0;
    f->eyes_up = false;
    f->blush = false;
    f->alert_border = false;
    f->bg = C_BG;
    f->bubble_radius = 0;

    switch (state)
    {
    case SIMI_STATE_BOOT:
    case SIMI_STATE_HAPPY:
        f->mouth_curve = 85;
        f->blush = true;
        break;
    case SIMI_STATE_IDLE:
        f->mouth_curve = 30;
        break;
    case SIMI_STATE_LISTENING:
        f->mouth_curve = 45;
        f->brow_angle = 10; // cejas atentas, ligeramente alzadas
        break;
    case SIMI_STATE_THINKING:
        f->mouth_curve = 5;
        f->eyes_up = true;
        f->brow_angle = -10;
        break;
    case SIMI_STATE_TALKING:
        f->mouth_curve = 50;
        f->mouth_open = 60;
        break;
    case SIMI_STATE_MUTED:
    case SIMI_STATE_SLEEP:
        f->eye_open = 0; // ojos cerrados
        f->mouth_curve = 15;
        f->mouth_open = 10;
        break;
    case SIMI_STATE_ALERT:
        f->mouth_curve = -85;  // boca severa hacia abajo
        f->brow_angle = 100;   // cejas muy enojadas
        f->eye_open = 55;
        f->alert_border = true;
        f->bg = C_ALERT_BG;
        break;
    case SIMI_STATE_SAD:
        f->mouth_curve = -55;
        f->brow_angle = -70; // cejas tristes (internas arriba)
        f->eye_open = 70;
        break;
    default:
        break;
    }
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Face renderer                                                             */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Calcula el semi-ancho real del torso (silueta) en una fila Y absoluta.
 *
 *        El cuerpo se construye como la UNION de 5 primitivas (rect + 2 triángulos
 *        de hombro + 2 elipses de pecho/vientre). Esta función replica esa misma
 *        unión en forma de fórmula, para poder recortar elementos verticales
 *        (como las franjas del Barça) a la curva real del cuerpo en vez de usar
 *        rectángulos rectos que sobresalen del contorno redondeado.
 */
static int simi_torso_half_width(int y_abs, int by)
{
    int yrel = y_abs - by;
    float w_taper = 0.0f, w1 = 0.0f, w2 = 0.0f;

    if (yrel >= 135 && yrel <= 160)
    {
        w_taper = 30.0f + (float)(yrel - 135) * (50.0f - 30.0f) / (160.0f - 135.0f);
    }
    if (yrel >= 140 && yrel <= 240)
    {
        float dy = (float)(yrel - 190);
        float t = 1.0f - (dy * dy) / (50.0f * 50.0f);
        if (t > 0.0f) w2 = 80.0f * sqrtf(t);
    }
    if (yrel >= 160 && yrel <= 300)
    {
        float dy = (float)(yrel - 230);
        float t = 1.0f - (dy * dy) / (70.0f * 70.0f);
        if (t > 0.0f) w1 = 120.0f * sqrtf(t);
    }

    float w = w_taper;
    if (w1 > w) w = w1;
    if (w2 > w) w = w2;
    return (int)w;
}

static void draw_body(simi_canvas_t *cv)
{
    int by = 15;

    switch (s_active_outfit)
    {
    case OUTFIT_FC_BARCELONA:
        // --- Sombras traseras ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 126, 76, SIMI_RGB(0, 50, 100));
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 84, 54, SIMI_RGB(0, 50, 100));
        canvas_fill_rect(cv, SIMI_CX - 34, 135 + by, 68, 25, SIMI_RGB(0, 50, 100));
        canvas_fill_triangle(cv, SIMI_CX - 34, 135 + by, SIMI_CX - 54, 160 + by, SIMI_CX - 34, 160 + by, SIMI_RGB(0, 50, 100));
        canvas_fill_triangle(cv, SIMI_CX + 34, 135 + by, SIMI_CX + 54, 160 + by, SIMI_CX + 34, 160 + by, SIMI_RGB(0, 50, 100));

        // --- Capa 1: Base Geometry (Cuerpo Azul) ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 120, 70, SIMI_RGB(0, 77, 152));
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 80, 50, SIMI_RGB(0, 77, 152));
        canvas_fill_rect(cv, SIMI_CX - 30, 135 + by, 60, 25, SIMI_RGB(0, 77, 152));
        canvas_fill_triangle(cv, SIMI_CX - 30, 135 + by, SIMI_CX - 50, 160 + by, SIMI_CX - 30, 160 + by, SIMI_RGB(0, 77, 152));
        canvas_fill_triangle(cv, SIMI_CX + 30, 135 + by, SIMI_CX + 50, 160 + by, SIMI_CX + 30, 160 + by, SIMI_RGB(0, 77, 152));

        // --- Capa 1: Franjas Granates (Más anchas: 44px) ---
        for (int yy = 135 + by; yy < cv->h; yy++)
        {
            int hw = simi_torso_half_width(yy, by);
            if (hw <= 0) continue;

            // Franja Izquierda (-62 a -18) -> Centro exacto en -40
            int clx0 = SIMI_CX - 62;
            int clx1 = SIMI_CX - 18;
            if (clx0 < SIMI_CX - hw) clx0 = SIMI_CX - hw; // Evita el bleed en hombros
            if (clx1 > clx0) {
                canvas_fill_rect(cv, clx0, yy, clx1 - clx0, 1, SIMI_RGB(165, 0, 68));
            }

            // Franja Derecha (+18 a +62) -> Centro exacto en +40
            int crx0 = SIMI_CX + 18;
            int crx1 = SIMI_CX + 62;
            if (crx1 > SIMI_CX + hw) crx1 = SIMI_CX + hw; // Evita el bleed en hombros
            if (crx1 > crx0) {
                canvas_fill_rect(cv, crx0, yy, crx1 - crx0, 1, SIMI_RGB(165, 0, 68));
            }
        }

        // --- NUEVA Capa: Costuras de Hombro (Diagonal hacia adentro \  / ) ---
        // Iniciamos AFUERA en el hombro (X = ±90) y bajamos HACIA ADENTRO (X = ±65).
        // Las franjas rojas están en ±62, así que dejamos 3 píxeles de seguridad para no tocarlas.
        canvas_thick_line(cv, SIMI_CX - 82, 170 + by, SIMI_CX - 72, 220 + by, 1, SIMI_RGB(0, 0, 0)); // Costura Izquierda (\)
        canvas_thick_line(cv, SIMI_CX + 82, 170 + by, SIMI_CX + 72, 220 + by, 1, SIMI_RGB(0, 0, 0)); // Costura Derecha (/)

        // --- Capa 2: Etiqueta Senyera y Paréntesis del Cuello ---
        
        // 1. El Paréntesis de la camiseta (Ajuste de ancho y altura)
        // Establecemos el grosor de la linea curva en 4px
        canvas_smile(cv, SIMI_CX, 150 + by, 58, 20, 4, SIMI_RGB(0, 77, 152));
        // Costura negra del cuello (debajo del azul)
        canvas_smile(cv, SIMI_CX, 152 + by, 58, 20, 1, SIMI_RGB(0, 0, 0));
        // 2. Etiqueta Senyera (Subimos a 164+by para que se ancle bien detrás de la barbilla)
        canvas_fill_rect(cv, SIMI_CX - 6, 170 + by, 12, 12, SIMI_RGB(237, 187, 0));
        canvas_fill_rect(cv, SIMI_CX - 4, 170 + by, 2, 12, SIMI_RGB(165, 0, 68));
        canvas_fill_rect(cv, SIMI_CX,     170 + by, 2, 12, SIMI_RGB(165, 0, 68));
        canvas_fill_rect(cv, SIMI_CX + 4, 170 + by, 2, 12, SIMI_RGB(165, 0, 68));
        // --- Escudo Procedural DETALLADO (Centrado en +40) ---
        {
            int ex = SIMI_CX + 28; // Anclado para que el centro del escudo quede en +40
            int ey = 188 + by;     

            // Base dorada (Senyera)
            canvas_fill_rect(cv, ex, ey, 24, 28, SIMI_RGB(255, 204, 0));

            // Cuadrante Superior Izquierdo (Cruz de San Jorge)
            canvas_fill_rect(cv, ex, ey, 12, 12, SIMI_RGB(255, 255, 255));
            canvas_fill_rect(cv, ex + 4, ey, 4, 12, SIMI_RGB(200, 30, 40)); 
            canvas_fill_rect(cv, ex, ey + 4, 12, 4, SIMI_RGB(200, 30, 40)); 

            // Cuadrante Superior Derecho (Senyera)
            canvas_fill_rect(cv, ex + 14, ey, 2, 12, SIMI_RGB(200, 30, 40));
            canvas_fill_rect(cv, ex + 18, ey, 2, 12, SIMI_RGB(200, 30, 40));
            canvas_fill_rect(cv, ex + 22, ey, 2, 12, SIMI_RGB(200, 30, 40));

            // Mitad Inferior (Blaugrana)
            canvas_fill_rect(cv, ex, ey + 12, 24, 16, SIMI_RGB(0, 77, 152));
            canvas_fill_rect(cv, ex + 4, ey + 12, 4, 16, SIMI_RGB(165, 0, 68));
            canvas_fill_rect(cv, ex + 12, ey + 12, 4, 16, SIMI_RGB(165, 0, 68));
            canvas_fill_rect(cv, ex + 20, ey + 12, 4, 16, SIMI_RGB(165, 0, 68));

            // Balón procedural al centro 
            canvas_fill_circle(cv, ex + 12, ey + 18, 4, SIMI_RGB(255, 204, 0));
        }

        // --- Texto "BARÇA" (Centrado en -40) ---
        {
            // Con 38px de ancho total, para centrarlo en -40 debe empezar en -59
            int startX = SIMI_CX - 59; 
            int by_t = 192 + by;
            // B (offset 0)
            canvas_fill_rect(cv, startX, by_t, 2, 10, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 2, by_t, 3, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 2, by_t + 4, 3, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 2, by_t + 8, 3, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 4, by_t + 2, 2, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 4, by_t + 6, 2, 2, SIMI_RGB(255, 204, 0));

            // A (offset +8)
            canvas_fill_rect(cv, startX + 8, by_t + 2, 2, 8, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 12, by_t + 2, 2, 8, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 10, by_t, 2, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 10, by_t + 4, 2, 2, SIMI_RGB(255, 204, 0));

            // R (offset +16)
            canvas_fill_rect(cv, startX + 16, by_t, 2, 10, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 18, by_t, 3, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 18, by_t + 4, 3, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 20, by_t + 2, 2, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 20, by_t + 6, 2, 4, SIMI_RGB(255, 204, 0));

            // Ç (offset +24)
            canvas_fill_rect(cv, startX + 24, by_t + 2, 2, 6, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 26, by_t, 4, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 26, by_t + 8, 4, 2, SIMI_RGB(255, 204, 0));
            // Cedilla
            canvas_fill_rect(cv, startX + 26, by_t + 10, 2, 2, SIMI_RGB(255, 204, 0));

            // A (offset +32)
            canvas_fill_rect(cv, startX + 32, by_t + 2, 2, 8, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 36, by_t + 2, 2, 8, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 34, by_t, 2, 2, SIMI_RGB(255, 204, 0));
            canvas_fill_rect(cv, startX + 34, by_t + 4, 2, 2, SIMI_RGB(255, 204, 0));
        }
        break;

    case OUTFIT_DOCTOR_WHITE:
    default:
        // --- Sombras traseras ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 126, 76, C_COAT_SH);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 84, 54, C_COAT_SH);
        
        // Sombra del cuello
        canvas_fill_rect(cv, SIMI_CX - 34, 135 + by, 68, 25, C_COAT_SH);
        canvas_fill_triangle(cv, SIMI_CX - 34, 135 + by, SIMI_CX - 54, 160 + by, SIMI_CX - 34, 160 + by, C_COAT_SH);
        canvas_fill_triangle(cv, SIMI_CX + 34, 135 + by, SIMI_CX + 54, 160 + by, SIMI_CX + 34, 160 + by, C_COAT_SH);

        // --- Cuerpo Principal (Abrigo Blanco) ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 120, 70, C_COAT);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 80, 50, C_COAT);
        canvas_fill_rect(cv, SIMI_CX - 30, 135 + by, 60, 25, C_COAT);
        canvas_fill_triangle(cv, SIMI_CX - 30, 135 + by, SIMI_CX - 50, 160 + by, SIMI_CX - 30, 160 + by, C_COAT);
        canvas_fill_triangle(cv, SIMI_CX + 30, 135 + by, SIMI_CX + 50, 160 + by, SIMI_CX + 30, 160 + by, C_COAT);

        // --- Ropa Interior ---
        canvas_fill_triangle(cv, SIMI_CX, 196 + by, SIMI_CX - 32, 146 + by, SIMI_CX + 32, 146 + by, C_SHIRT);
        canvas_fill_triangle(cv, SIMI_CX, 182 + by, SIMI_CX - 12, 156 + by, SIMI_CX + 12, 156 + by, C_TIE);
        canvas_fill_triangle(cv, SIMI_CX - 10, 178 + by, SIMI_CX + 10, 178 + by, SIMI_CX, 210 + by, C_TIE);

        // --- Capa 3: Logotipo de Pecho Izquierdo Apilado (RE-POSICIONADO INWARD) ---
        // Stacking text above existing cross, using C_TIE color for text as requested.
        // All elements are stacked and center-aligned on X=SIMI_CX+52 (prev was +68).
        
        uint16_t logo_color = C_TIE; // Tie color used for text
        int text_startY = 176 + by; // Just below barbilla occlusion limit.

        // --- Linea 1: "Dr." --- (Centrado en X=SIMI_CX+52)
        {
            int drX = SIMI_CX + 44; // D stem starts at ~44 relative (centers logo approx at +52)
            int by_t = text_startY;
            
            // D (caps pixel block - consistent with wide grid/KISS logic)
            canvas_fill_rect(cv, drX, by_t, 2, 10, logo_color); // vertical stem
            canvas_fill_rect(cv, drX + 2, by_t, 3, 2, logo_color); // top
            canvas_fill_rect(cv, drX + 2, by_t + 8, 3, 2, logo_color); // bottom
            canvas_fill_rect(cv, drX + 5, by_t + 2, 2, 6, logo_color); // curve/right-stem
            
            // lowercase 'r' (compact procedural)
            canvas_fill_rect(cv, drX + 8, by_t + 4, 2, 6, logo_color); // stem
            canvas_fill_rect(cv, drX + 10, by_t + 4, 2, 2, logo_color); // serif top-right

            // dot '.'
            canvas_fill_rect(cv, drX + 13, by_t + 8, 2, 2, logo_color); // dot
        }

        // --- Linea 2: "Simi" --- (Centrado en X=SIMI_CX+52)
        {
            int simiX = SIMI_CX + 41; // S starts at ~41 relative (centers logo approx at +52)
            int by_t = text_startY + 12; // Standard 2px vertical spacing below Dr. (Dr. ends at tY+10)

            // S (caps pixel block)
            canvas_fill_rect(cv, simiX, by_t, 5, 2, logo_color); // top
            canvas_fill_rect(cv, simiX, by_t, 2, 4, logo_color); // top-left stem (overlapping top)
            canvas_fill_rect(cv, simiX, by_t + 4, 5, 2, logo_color); // middle
            canvas_fill_rect(cv, simiX + 3, by_t + 4, 2, 4, logo_color); // bottom-right stem (overlapping middle)
            canvas_fill_rect(cv, simiX, by_t + 8, 5, 2, logo_color); // bottom
            
            // lowercase 'i'
            canvas_fill_rect(cv, simiX + 6, by_t, 2, 2, logo_color); // dot
            canvas_fill_rect(cv, simiX + 6, by_t + 4, 2, 6, logo_color); // stem
            
            // lowercase 'm' (simplified pixel font 'm' adapted previously for Barça wide grid logic)
            canvas_fill_rect(cv, simiX + 9, by_t + 4, 2, 6, logo_color); // left stem
            canvas_fill_rect(cv, simiX + 13, by_t + 4, 2, 6, logo_color); // middle stem
            canvas_fill_rect(cv, simiX + 17, by_t + 4, 2, 6, logo_color); // right stem
            canvas_fill_rect(cv, simiX + 10, by_t + 2, 3, 2, logo_color); // top-left arc
            canvas_fill_rect(cv, simiX + 14, by_t + 2, 3, 2, logo_color); // top-right arc
            
            // lowercase 'i' (repeat)
            canvas_fill_rect(cv, simiX + 20, by_t, 2, 2, logo_color); // dot
            canvas_fill_rect(cv, simiX + 20, by_t + 4, 2, 6, logo_color); // stem
        }

        // --- Linea 3: Cruz medica roja (Moved lower and stacked, centered in logic) ---
        // Stacking text above standard cross placement (original: 184), moving cross down for clear logo look.
        // Also center-aligning this to CX+52.
        int cy_cross = 212 + by; // Standard size cross moved lower than original to prevent clipping.
        int cx_cross = SIMI_CX + 52; // new precision X anchor area to stack.
        
        canvas_fill_rect(cv, cx_cross - 2, cy_cross - 7, 4, 16, C_CROSS); // vertical bar (size 4x16 standard).
        canvas_fill_rect(cv, cx_cross - 7, cy_cross - 2, 16, 4, C_CROSS); // horizontal bar (size 16x4 standard).
        
        break;
    
    case OUTFIT_CHAPULIN_RED:
        // --- Sombras traseras ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 126, 76, C_CHAPULIN_SH);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 84, 54, C_CHAPULIN_SH);
        canvas_fill_rect(cv, SIMI_CX - 34, 135 + by, 68, 25, C_CHAPULIN_SH);
        canvas_fill_triangle(cv, SIMI_CX - 34, 135 + by, SIMI_CX - 54, 160 + by, SIMI_CX - 34, 160 + by, C_CHAPULIN_SH);
        canvas_fill_triangle(cv, SIMI_CX + 34, 135 + by, SIMI_CX + 54, 160 + by, SIMI_CX + 34, 160 + by, C_CHAPULIN_SH);

        // --- Cuerpo Principal (Traje Rojo) ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 120, 70, C_CHAPULIN_RED);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 80, 50, C_CHAPULIN_RED);
        canvas_fill_rect(cv, SIMI_CX - 30, 135 + by, 60, 25, C_CHAPULIN_RED);
        canvas_fill_triangle(cv, SIMI_CX - 30, 135 + by, SIMI_CX - 50, 160 + by, SIMI_CX - 30, 160 + by, C_CHAPULIN_RED);
        canvas_fill_triangle(cv, SIMI_CX + 30, 135 + by, SIMI_CX + 50, 160 + by, SIMI_CX + 30, 160 + by, C_CHAPULIN_RED);

        // --- Emblema Corazón Amarillo ---
        canvas_fill_circle(cv, SIMI_CX - 30, 205 + by, 35, C_CHAPULIN_YELLOW);
        canvas_fill_circle(cv, SIMI_CX + 30, 205 + by, 35, C_CHAPULIN_YELLOW);
        canvas_fill_triangle(cv, SIMI_CX - 62, 217 + by, SIMI_CX + 62, 217 + by, SIMI_CX, 295 + by, C_CHAPULIN_YELLOW);

        // --- Letras "CH" en Rojo (Subidas +10px respecto al centro del corazón) ---
        int ch_y = 210 + by; 
        // Letra C 
        canvas_fill_rect(cv, SIMI_CX - 20, ch_y - 10, 18, 4, C_CHAPULIN_RED); // top
        canvas_fill_rect(cv, SIMI_CX - 20, ch_y - 10, 4, 20, C_CHAPULIN_RED); // left
        canvas_fill_rect(cv, SIMI_CX - 20, ch_y + 6, 18, 4, C_CHAPULIN_RED);  // bottom

        // Letra H 
        canvas_fill_rect(cv, SIMI_CX + 2, ch_y - 10, 4, 20, C_CHAPULIN_RED);  // left
        canvas_fill_rect(cv, SIMI_CX + 16, ch_y - 10, 4, 20, C_CHAPULIN_RED); // right
        canvas_fill_rect(cv, SIMI_CX + 2, ch_y - 2, 18, 4, C_CHAPULIN_RED);   // mid
        break;

    case OUTFIT_SELECCION_GREEN:
        // --- Sombras traseras ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 126, 76, C_JERSEY_SH);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 84, 54, C_JERSEY_SH);
        canvas_fill_rect(cv, SIMI_CX - 34, 135 + by, 68, 25, C_JERSEY_SH);
        canvas_fill_triangle(cv, SIMI_CX - 34, 135 + by, SIMI_CX - 54, 160 + by, SIMI_CX - 34, 160 + by, C_JERSEY_SH);
        canvas_fill_triangle(cv, SIMI_CX + 34, 135 + by, SIMI_CX + 54, 160 + by, SIMI_CX + 34, 160 + by, C_JERSEY_SH);

        // --- Cuerpo Principal (Jersey Verde) ---
        canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 120, 70, C_JERSEY_GREEN);
        canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 80, 50, C_JERSEY_GREEN);
        canvas_fill_rect(cv, SIMI_CX - 30, 135 + by, 60, 25, C_JERSEY_GREEN);
        canvas_fill_triangle(cv, SIMI_CX - 30, 135 + by, SIMI_CX - 50, 160 + by, SIMI_CX - 30, 160 + by, C_JERSEY_GREEN);
        canvas_fill_triangle(cv, SIMI_CX + 30, 135 + by, SIMI_CX + 50, 160 + by, SIMI_CX + 30, 160 + by, C_JERSEY_GREEN);

        // --- Cuello V-Neck Tricolor ---
        canvas_fill_triangle(cv, SIMI_CX, 225 + by, SIMI_CX - 45, 151 + by, SIMI_CX + 45, 151 + by, SIMI_RGB(255, 255, 255)); 
        canvas_fill_triangle(cv, SIMI_CX, 215 + by, SIMI_CX - 35, 151 + by, SIMI_CX + 35, 151 + by, C_JERSEY_RED); 
        canvas_fill_triangle(cv, SIMI_CX, 205 + by, SIMI_CX - 25, 151 + by, SIMI_CX + 25, 151 + by, C_SKIN); 

        // --- Escudo Nacional (Pecho Izquierdo, derecha en la pantalla) ---
        canvas_fill_rect(cv, SIMI_CX + 45, 187 + by, 8, 14, SIMI_RGB(0, 100, 30)); // Verde oscuro
        canvas_fill_rect(cv, SIMI_CX + 53, 187 + by, 8, 14, SIMI_RGB(255, 255, 255)); // Blanco
        canvas_fill_rect(cv, SIMI_CX + 61, 187 + by, 8, 14, C_JERSEY_RED); // Rojo
        // El Águila y el Nopal (Detalle café oscuro al centro de la franja blanca)
        canvas_fill_rect(cv, SIMI_CX + 55, 192 + by, 4, 4, SIMI_RGB(139, 69, 19)); 
        break;
    }
}

static void draw_eye(simi_canvas_t *cv, int ex, int ey, const simi_face_t *f)
{
    int ry = 16 * f->eye_open / 100;
    int pupil_dy = f->eyes_up ? -4 : 0;
    if (ry < 1)
    {
        // Ojo cerrado: línea curva (párpado)
        canvas_smile(cv, ex, ey, 14, 4, 3, C_EYE);
        return;
    }
    // Esclerótica blanca
    canvas_fill_ellipse(cv, ex, ey, 12, ry, C_EYE_HI);
    // Pupila negra
    canvas_fill_ellipse(cv, ex, ey + pupil_dy, 6, (ry * 3) / 4, C_EYE);
    // Brillo blanco
    if (ry >= 7)
    {
        canvas_fill_circle(cv, ex + 2, ey + pupil_dy - 2, 2, C_EYE_HI);
    }
}


static inline bool is_in_teardrop(int dx, int dy, int r)
{
    if (r <= 0) return false;
    if (dy >= 0) {
        return (dx * dx + dy * dy) <= (r * r);
    } else {
        return (dx * dx * r) <= (r * r - dy * dy) * (r + dy);
    }
}

static void simi_draw_teardrop(int cx, int cy, int r)
{
    simi_canvas_t *cv = &s_cv;
    if (r <= 0) return;
    
    int y_start = cy - r;
    if (y_start < 0) y_start = 0;
    int y_end = cy + r;
    if (y_end >= cv->h) y_end = cv->h - 1;
    
    int x_start = cx - r;
    if (x_start < 0) x_start = 0;
    int x_end = cx + r;
    if (x_end >= cv->w) x_end = cv->w - 1;

    uint16_t c_border = SIMI_RGB(120, 190, 220);
    uint16_t c_highlight = SIMI_RGB(255, 255, 255);
    uint16_t c_interior = SIMI_RGB(200, 230, 255);

    for (int y = y_start; y <= y_end; y++)
    {
        for (int x = x_start; x <= x_end; x++)
        {
            int dx = x - cx;
            int dy_ = y - cy;
            
            if (is_in_teardrop(dx, dy_, r))
            {
                if (!is_in_teardrop(dx, dy_, r - 1))
                {
                    // Rule A: Border - Opaque stroke
                    cv->buf[y * cv->w + x] = c_border;
                }
                else if (r > 4 && dx > 1 && dy_ < -1 && !is_in_teardrop(dx, dy_, r - 3))
                {
                    // Rule B: Specular Highlight - Opaque pure white crescent
                    cv->buf[y * cv->w + x] = c_highlight;
                }
                else
                {
                    // Rule C: Interior - Flat pastel fill
                    cv->buf[y * cv->w + x] = c_interior;
                }
            }
        }
    }
}

/**
 * @brief Renders the full face described by @p f into the module canvas.
 */
static void simi_render(const simi_face_t *f)
{
    simi_canvas_t *cv = &s_cv;
    const int dy = f->head_dy + 15;
    const bool alert_face = f->alert_border && f->brow_angle >= 80;

    if (!s_bg_rendered)
    {
        canvas_clear(cv, f->bg);

        // Cuerpo primero (queda detrás del mentón)
        draw_body(cv);

        // ── Cabeza (Forma de domo ancho) ──
        // Para lograrlo, usamos dos elipses superpuestas: una más achatada y ancha arriba, y las mejillas abajo
        canvas_fill_ellipse(cv, SIMI_CX, 78 + dy, 70, 48, C_SKIN); // Frente/Cráneo (más ancho y achatado)
        canvas_fill_ellipse(cv, SIMI_CX, 110 + dy, 78, 58, C_SKIN); // Mejillas/Mandíbula (ligeramente más anchas)
        
        // Orejas (bajadas ligeramente para coincidir con la parte ancha)
        canvas_fill_circle(cv, SIMI_CX - 76, 108 + dy, 14, C_SKIN);
        canvas_fill_circle(cv, SIMI_CX + 76, 108 + dy, 14, C_SKIN);

        // ── Los 3 cabellos a cada lado (más gruesos, blancos y como patitas de araña) ──
        // Movidos ligeramente hacia afuera para coincidir con la nueva cabeza ancha
        // Izquierda (salen hacia arriba/afuera y luego caen)
        canvas_thick_line(cv, SIMI_CX - 66, 66 + dy, SIMI_CX - 80, 60 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX - 80, 60 + dy, SIMI_CX - 92, 68 + dy, 4, C_HAIR);

        canvas_thick_line(cv, SIMI_CX - 68, 80 + dy, SIMI_CX - 82, 76 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX - 82, 76 + dy, SIMI_CX - 94, 84 + dy, 4, C_HAIR);

        canvas_thick_line(cv, SIMI_CX - 72, 94 + dy, SIMI_CX - 84, 92 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX - 84, 92 + dy, SIMI_CX - 94, 102 + dy, 4, C_HAIR);

        // Derecha (salen hacia arriba/afuera y luego caen)
        canvas_thick_line(cv, SIMI_CX + 66, 66 + dy, SIMI_CX + 80, 60 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX + 80, 60 + dy, SIMI_CX + 92, 68 + dy, 4, C_HAIR);

        canvas_thick_line(cv, SIMI_CX + 68, 80 + dy, SIMI_CX + 82, 76 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX + 82, 76 + dy, SIMI_CX + 94, 84 + dy, 4, C_HAIR);

        canvas_thick_line(cv, SIMI_CX + 72, 94 + dy, SIMI_CX + 84, 92 + dy, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX + 84, 92 + dy, SIMI_CX + 94, 102 + dy, 4, C_HAIR);

        if (s_static_bg_buf != NULL)
        {
            memcpy(s_static_bg_buf, cv->buf, SIMI_CANVAS_W * SIMI_CANVAS_H * sizeof(uint16_t));
        }
        s_bg_rendered = true;

        // T3 FIX: Prevent dynamic tongue/mouth from being permanently erased by static snapshot.
        // Force the dirty-rect logic to register an open mouth so it gets properly layered.
        if (s_anim_speaking)
        {
            ((simi_face_t *)f)->mouth_open = 100;
        }
    }
    else if (s_static_bg_buf != NULL)
    {
        // T3: Dynamic Dirty Rect Restore (The Eraser)
        bool full_redraw_bypass = f->alert_border || 
            (cv->clip_x0 == 0 && cv->clip_y0 == 0 && cv->clip_x1 == cv->w - 1 && cv->clip_y1 == cv->h - 1);
            
        if (full_redraw_bypass)
        {
            // Risk 2 Mitigation: Full memcpy for global overlays or full refresh
            memcpy(cv->buf, s_static_bg_buf, SIMI_CANVAS_W * SIMI_CANVAS_H * sizeof(uint16_t));
        }
        else
        {
            // Targeted row-by-row copy for facial dirty rect (X=60, Y=30, W=200, H=140)
            int start_x = 60, start_y = 30, w = 200, h = 140;
            for (int r = start_y; r < start_y + h; r++) {
                memcpy(&cv->buf[r * cv->w + start_x], 
                       &s_static_bg_buf[r * cv->w + start_x], 
                       w * sizeof(uint16_t));
            }
        }
    }

    // ── Cejas (blancas y arqueadas para dar expresión de felicidad) ──
    int browY = 66 + dy;
    if (alert_face)
    {
        canvas_thick_line(cv, SIMI_CX - 42, browY - 6, SIMI_CX - 9, browY + 8, 6, C_ALERT);
        canvas_thick_line(cv, SIMI_CX + 9, browY + 8, SIMI_CX + 42, browY - 6, 6, C_ALERT);
        canvas_thick_line(cv, SIMI_CX - 40, browY - 8, SIMI_CX - 8, browY + 6, 4, C_HAIR);
        canvas_thick_line(cv, SIMI_CX + 8, browY + 6, SIMI_CX + 40, browY - 8, 4, C_HAIR);
    }
    else
    {
        int brow_arc = -6 - (f->brow_angle * 4 / 100); // curvar hacia arriba
        canvas_smile(cv, SIMI_CX - 24, browY, 14, brow_arc, 5, C_HAIR);
        canvas_smile(cv, SIMI_CX + 24, browY, 14, brow_arc, 5, C_HAIR);
    }

    // ── Ojos ──
    int eyeY = 88 + dy;
    draw_eye(cv, SIMI_CX - 22, eyeY, f);
    draw_eye(cv, SIMI_CX + 22, eyeY, f);

    // ── Mejillas (blush) ──
    if (f->blush)
    {
        canvas_fill_ellipse(cv, SIMI_CX - 46, 116 + dy, 11, 6, C_BLUSH);
        canvas_fill_ellipse(cv, SIMI_CX + 46, 116 + dy, 11, 6, C_BLUSH);
    }

    // ── Nariz ──
    canvas_fill_ellipse(cv, SIMI_CX, 108 + dy, 10, 14, C_SKIN_SH);

    // ── Boca (se dibuja ANTES del bigote para que quede debajo) ──
    int mouthY = 142 + dy;
    // Siempre dibujaremos la boca abierta (fondo oscuro) con la lengua en el fondo
    // La boca es una elipse oscura. Su parte superior quedará oculta por el bigote,
    // dejando ver solo la "D" acostada típica de una sonrisa abierta.
    int mo = alert_face
                 ? (5 + (f->mouth_open * 10 / 100))
                 : (12 + (f->mouth_open * 10 / 100));
    
    // Cavidad de la boca (oscura)
    if (!alert_face || f->mouth_open > 0)
    {
        canvas_fill_ellipse(cv, SIMI_CX, mouthY, alert_face ? 17 : 20, mo, C_MOUTH_OPEN);
    }
    
    // Lengua (elipse rosa/roja en la parte inferior)
    if (!alert_face && s_anim_speaking && f->mouth_open > 10) {
        canvas_fill_ellipse(cv, SIMI_CX, mouthY + mo - 4, 12, 6, C_TONGUE);
    }

    // ── Bigote blanco enorme (rasgo icónico, tapa parte de la boca) ──
    // Dos elipses centrales (más delgadas para no tapar tanto la boca)
    canvas_fill_ellipse(cv, SIMI_CX - 20, 128 + dy, 26, 14, C_HAIR);
    canvas_fill_ellipse(cv, SIMI_CX + 20, 128 + dy, 26, 14, C_HAIR);
    // Puntas redondeadas cayendo hacia los lados y un poco hacia abajo
    canvas_fill_ellipse(cv, SIMI_CX - 40, 132 + dy, 16, 12, C_HAIR);
    canvas_fill_ellipse(cv, SIMI_CX + 40, 132 + dy, 16, 12, C_HAIR);

    if (alert_face && f->mouth_open == 0)
    {
        canvas_smile(cv, SIMI_CX, mouthY + 6, 17, -9, 4, C_MOUTH_OPEN);
    }

    // ── Marco de alerta (Phase 1: dentro del propio retrato) ──
    if (f->alert_border)
    {
        const int t = 6;
        canvas_fill_rect(cv, 0, 0, cv->w, t, C_ALERT);
        canvas_fill_rect(cv, 0, cv->h - t, cv->w, t, C_ALERT);
        canvas_fill_rect(cv, 0, 0, t, cv->h, C_ALERT);
        canvas_fill_rect(cv, cv->w - t, 0, t, cv->h, C_ALERT);
    }

    // ── Zócalo de Subtítulos (Option B) ──
    if (s_simi_overlay_text[0] != '\0' && cv->clip_y1 >= cv->h - 40)
    {
        int bar_h = 40;
        int bar_y = cv->h - bar_h;
        canvas_fill_rect(cv, 0, bar_y, cv->w, bar_h, SIMI_RGB(20, 20, 20));

        int text_scale = 1;
        int text_width = ui_get_text_width(s_simi_overlay_text, text_scale);
        if (text_width < cv->w - 20) text_scale = 2;
        text_width = ui_get_text_width(s_simi_overlay_text, text_scale);
        if (text_width > cv->w - 10) text_scale = 1;

        int text_height = 8 * text_scale;
        int bx = (cv->w - text_width) / 2;
        int by = bar_y + (bar_h - text_height) / 2;

        ui_draw_text_to_buffer(cv->buf, cv->w, cv->h, bx, by, s_simi_overlay_text, s_simi_overlay_color, text_scale);
    }

    // ── Zócalo de Top-Right (e.g. Mute Timer) ──
    if (s_simi_top_right_text[0] != '\0')
    {
        int text_scale = 1;
        int text_width = ui_get_text_width(s_simi_top_right_text, text_scale);
        int text_height = 8 * text_scale;
        
        int pad_left = 6;
        int pad_right = 6;
        int pad_top = 4;
        int pad_bottom = 4;
        
        // Match a fixed height of 16px
        int target_icon_h = 16;
        
        int badge_w = pad_left + text_width + pad_right;
        int badge_h = (text_height > target_icon_h ? text_height : target_icon_h) + pad_top + pad_bottom;
        
        int badge_x = cv->w - badge_w - 15;
        int badge_y = 20;
        
        // Background Badge (Dark Slate Gray)
        uint16_t c_badge = SIMI_RGB(30, 30, 35);
        canvas_fill_round_rect(cv, badge_x, badge_y, badge_w, badge_h, 4, c_badge);
        
        // Text
        int text_x = badge_x + pad_left;
        int text_y = badge_y + (badge_h - text_height) / 2;
        ui_draw_text_to_buffer(cv->buf, cv->w, cv->h, text_x, text_y, s_simi_top_right_text, SIMI_RGB(255, 255, 255), text_scale);
    }

    // ── Procedural Snot Bubble (Teardrop) (drawn at the very end to sit on top of everything) ──
    if (f->bubble_radius > 0)
    {
        int bubble_x = SIMI_CX + 6; // Position slightly offset from the nose
        int bubble_y = 112 + dy + f->bubble_radius; // Anchor the tip (cy - r) dynamically to the nose
        simi_draw_teardrop(bubble_x, bubble_y, f->bubble_radius);
    }
}

void ui_simi_render_static(simi_state_t state)
{
    ui_simi_stop();

    if (!ui_simi_ready())
    {
        ESP_LOGW(TAG, "ui_simi_render_static llamado sin canvas inicializado");
        return;
    }

    simi_face_t f;
    simi_face_for_state(state, &f);
    simi_render(&f);

    (void)simi_blit_frame(true);
}