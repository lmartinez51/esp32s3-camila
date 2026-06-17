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

static const char *TAG = "SIMI";

/* ── Geometría del lienzo (pantalla completa 320×240) ── */
#define SIMI_CANVAS_W 320
#define SIMI_CANVAS_H 240
#define SIMI_BLIT_X 0
#define SIMI_BLIT_Y 0
#define SIMI_CX 160

#define SIMI_DIRTY_BUF_SIZE 16384
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

static simi_canvas_t s_cv = {0};
static bool s_simi_screen_cleared = false;
static bool s_simi_backlight_ready = false;

static char s_simi_overlay_text[32] = {0};
static uint16_t s_simi_overlay_color = 0;

static char s_simi_temperature_text[16] = {0};

static SemaphoreHandle_t s_anim_mutex = NULL;
static TaskHandle_t s_anim_task = NULL;
static bool s_anim_stop_requested = false;
static simi_state_t s_anim_state = SIMI_STATE_IDLE;
static bool s_anim_speaking = false;

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
    bool local_speaking = false;
    bool local_stop = (s_anim_mutex == NULL);

    if (s_anim_mutex != NULL &&
        xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        local_state = s_anim_state;
        local_speaking = s_anim_speaking;
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
        f->alert_border = ((frame & 1) == 0);
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
            static char last_temperature_text[16] = {0};
            bool text_changed = strncmp(s_simi_overlay_text, last_overlay_text, sizeof(last_overlay_text)) != 0;
            if (text_changed) strncpy(last_overlay_text, s_simi_overlay_text, sizeof(last_overlay_text));
            
            bool temp_changed = strncmp(s_simi_temperature_text, last_temperature_text, sizeof(last_temperature_text)) != 0;
            if (temp_changed) strncpy(last_temperature_text, s_simi_temperature_text, sizeof(last_temperature_text));

            bool full_redraw = (visual_state != last_visual_state) || (last_visual_state == SIMI_STATE_MAX);

            if (full_redraw)
            {
                s_cv.clip_x0 = 0; s_cv.clip_y0 = 0; s_cv.clip_x1 = s_cv.w - 1; s_cv.clip_y1 = s_cv.h - 1;
                simi_render(&f);
                simi_blit_dirty_rect(0, 0, s_cv.w - 1, s_cv.h - 1, blocking_blit);
            }
            else
            {
                bool head_moved = (f.head_dy != last_f.head_dy) || (f.bubble_radius != last_f.bubble_radius);
                bool eyes_changed = (f.eye_open != last_f.eye_open) || (f.eyes_up != last_f.eyes_up) || (f.brow_angle != last_f.brow_angle);
                bool mouth_changed = (f.mouth_open != last_f.mouth_open) || (f.mouth_curve != last_f.mouth_curve);

                if (head_moved)
                {
                    s_cv.clip_x0 = 40; s_cv.clip_y0 = 60; s_cv.clip_x1 = 280; s_cv.clip_y1 = 180;
                    simi_render(&f);
                    simi_blit_dirty_rect(s_cv.clip_x0, s_cv.clip_y0, s_cv.clip_x1, s_cv.clip_y1, blocking_blit);
                }
                else
                {
                    if (eyes_changed)
                    {
                        s_cv.clip_x0 = 80; s_cv.clip_y0 = 70; s_cv.clip_x1 = 240; s_cv.clip_y1 = 120;
                        simi_render(&f);
                        simi_blit_dirty_rect(s_cv.clip_x0, s_cv.clip_y0, s_cv.clip_x1, s_cv.clip_y1, blocking_blit);
                    }
                    if (mouth_changed)
                    {
                        s_cv.clip_x0 = 100; s_cv.clip_y0 = 120; s_cv.clip_x1 = 220; s_cv.clip_y1 = 180;
                        simi_render(&f);
                        simi_blit_dirty_rect(s_cv.clip_x0, s_cv.clip_y0, s_cv.clip_x1, s_cv.clip_y1, blocking_blit);
                    }
                }

                if (text_changed)
                {
                    s_cv.clip_x0 = 0; s_cv.clip_y0 = s_cv.h - 40; s_cv.clip_x1 = s_cv.w - 1; s_cv.clip_y1 = s_cv.h - 1;
                    simi_render(&f);
                    simi_blit_dirty_rect(s_cv.clip_x0, s_cv.clip_y0, s_cv.clip_x1, s_cv.clip_y1, blocking_blit);
                }

                if (temp_changed)
                {
                    s_cv.clip_x0 = 0; s_cv.clip_y0 = 0; s_cv.clip_x1 = 120; s_cv.clip_y1 = 40;
                    simi_render(&f);
                    simi_blit_dirty_rect(s_cv.clip_x0, s_cv.clip_y0, s_cv.clip_x1, s_cv.clip_y1, blocking_blit);
                }
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
    s_cv.buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_cv.buf)
    {
        ESP_LOGI(TAG, "Canvas %dx%d (%u B) en PSRAM", SIMI_CANVAS_W, SIMI_CANVAS_H, (unsigned)bytes);
    }
    else
    {
        s_cv.buf = NULL;
    }

    if (!s_cv.buf)
    {
        ESP_LOGE(TAG, "Fallo al asignar el canvas de Dr. Simi en PSRAM (%u B)", (unsigned)bytes);
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

void ui_simi_notify_speaking(bool active)
{
    if (simi_anim_ensure_mutex() != ESP_OK)
    {
        return;
    }

    TaskHandle_t task = NULL;
    bool changed = false;
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        changed = (s_anim_speaking != active);
        s_anim_speaking = active;
        task = s_anim_task;
        xSemaphoreGive(s_anim_mutex);
    }

    if (changed && task != NULL)
    {
        xTaskNotifyGive(task);
    }
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

void ui_simi_set_temperature_text(const char *text)
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
            if (strncmp(s_simi_temperature_text, text, sizeof(s_simi_temperature_text)) != 0)
            {
                strncpy(s_simi_temperature_text, text, sizeof(s_simi_temperature_text) - 1);
                s_simi_temperature_text[sizeof(s_simi_temperature_text) - 1] = '\0';
                changed = true;
            }
        }
        else
        {
            if (s_simi_temperature_text[0] != '\0')
            {
                s_simi_temperature_text[0] = '\0';
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

void ui_simi_deinit(void)
{
    ui_simi_stop();
    if (simi_anim_is_running())
    {
        ESP_LOGW(TAG, "Canvas de Dr. Simi conservado porque la tarea de animacion sigue activa");
        return;
    }

    if (s_cv.buf)
    {
        heap_caps_free(s_cv.buf);
        s_cv.buf = NULL;
    }
    if (s_dirty_buf)
    {
        heap_caps_free(s_dirty_buf);
        s_dirty_buf = NULL;
    }
    s_simi_screen_cleared = false;
    s_simi_backlight_ready = false;
    s_cv.w = s_cv.h = 0;
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

static void draw_body(simi_canvas_t *cv)
{
    int by = 15;

    // --- Sombras traseras ---
    canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 126, 76, C_COAT_SH);
    canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 84, 54, C_COAT_SH);
    
    // Sombra del cuello
    canvas_fill_rect(cv, SIMI_CX - 34, 135 + by, 68, 25, C_COAT_SH);
    canvas_fill_triangle(cv, SIMI_CX - 34, 135 + by, SIMI_CX - 54, 160 + by, SIMI_CX - 34, 160 + by, C_COAT_SH);
    canvas_fill_triangle(cv, SIMI_CX + 34, 135 + by, SIMI_CX + 54, 160 + by, SIMI_CX + 34, 160 + by, C_COAT_SH);

    // --- Cuerpo Principal (Abrigo Blanco) ---
    // 1. Cuerpo inferior (curva amplia hacia abajo)
    canvas_fill_ellipse(cv, SIMI_CX, 230 + by, 120, 70, C_COAT);
    
    // 2. Hombros medios (curva más estrecha para transición)
    canvas_fill_ellipse(cv, SIMI_CX, 190 + by, 80, 50, C_COAT);

    // 3. Pequeño cuello (diagonal recta)
    canvas_fill_rect(cv, SIMI_CX - 30, 135 + by, 60, 25, C_COAT);
    canvas_fill_triangle(cv, SIMI_CX - 30, 135 + by, SIMI_CX - 50, 160 + by, SIMI_CX - 30, 160 + by, C_COAT);
    canvas_fill_triangle(cv, SIMI_CX + 30, 135 + by, SIMI_CX + 50, 160 + by, SIMI_CX + 30, 160 + by, C_COAT);

    // --- Ropa Interior ---
    // Camisa en V
    canvas_fill_triangle(cv, SIMI_CX, 196 + by, SIMI_CX - 32, 146 + by, SIMI_CX + 32, 146 + by, C_SHIRT);

    // Corbata
    canvas_fill_triangle(cv, SIMI_CX, 182 + by, SIMI_CX - 12, 156 + by, SIMI_CX + 12, 156 + by, C_TIE);
    canvas_fill_triangle(cv, SIMI_CX - 10, 178 + by, SIMI_CX + 10, 178 + by, SIMI_CX, 210 + by, C_TIE);

    // Cruz médica roja
    canvas_fill_rect(cv, (SIMI_CX - 68) - 2, 184 + by - 7, 4, 16, C_CROSS);
    canvas_fill_rect(cv, (SIMI_CX - 68) - 7, 184 + by - 2, 16, 4, C_CROSS);
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

static void canvas_draw_thermometer_icon(simi_canvas_t *cv, int x, int y)
{
    // A simple 8x16 minimalist thermometer pattern.
    // 0 = Transparent, 1 = White (Glass), 2 = Red (Liquid), 3 = Dark Slate Gray (Empty)
    static const uint8_t pattern[16][8] = {
        {0, 0, 1, 1, 1, 1, 0, 0},
        {0, 1, 3, 3, 3, 3, 1, 0},
        {0, 1, 3, 3, 3, 3, 1, 0},
        {0, 1, 3, 3, 3, 3, 1, 0},
        {0, 1, 3, 3, 3, 3, 1, 0},
        {0, 1, 3, 3, 3, 3, 1, 0},
        {0, 1, 2, 2, 2, 2, 1, 0},
        {0, 1, 2, 2, 2, 2, 1, 0},
        {0, 1, 2, 2, 2, 2, 1, 0},
        {0, 1, 2, 2, 2, 2, 1, 0},
        {1, 2, 2, 2, 2, 2, 2, 1},
        {1, 2, 2, 2, 2, 2, 2, 1},
        {1, 2, 2, 2, 2, 2, 2, 1},
        {1, 2, 2, 2, 2, 2, 2, 1},
        {0, 1, 2, 2, 2, 2, 1, 0},
        {0, 0, 1, 1, 1, 1, 0, 0}
    };

    uint16_t palette[4] = {
        0,                            // 0 = Transparent (unused in draw)
        SIMI_RGB(255, 255, 255),      // 1 = White (Glass)
        SIMI_RGB(255, 40, 40),        // 2 = Red (Liquid)
        SIMI_RGB(30, 30, 35)          // 3 = Dark Slate Gray (Empty)
    };

    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t p = pattern[r][c];
            if (p == 0) continue; // Skip transparent pixels
            
            int px = x + c;
            int py = y + r;
            if (px >= 0 && py >= 0 && px < cv->w && py < cv->h) {
                cv->buf[py * cv->w + px] = palette[p];
            }
        }
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
    if (!alert_face && mo > 6) {
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

    // ── Zócalo de Temperatura (Top Left) ──
    if (s_simi_temperature_text[0] != '\0')
    {
        int text_scale = 1;
        int text_width = ui_get_text_width(s_simi_temperature_text, text_scale);
        int text_height = 8 * text_scale;
        
        int pad_left = 6;
        int pad_right = 6;
        int pad_mid = 4;
        int pad_top = 4;
        int pad_bottom = 4;
        
        int icon_w = 8;
        int icon_h = 16;
        
        int badge_w = pad_left + icon_w + pad_mid + text_width + pad_right;
        int badge_h = (text_height > icon_h ? text_height : icon_h) + pad_top + pad_bottom;
        
        int badge_x = 15;
        int badge_y = 20;
        
        // Background Badge (Dark Slate Gray)
        uint16_t c_badge = SIMI_RGB(30, 30, 35);
        canvas_fill_round_rect(cv, badge_x, badge_y, badge_w, badge_h, 4, c_badge);
        
        // Thermometer Icon
        int icon_y = badge_y + (badge_h - icon_h) / 2;
        canvas_draw_thermometer_icon(cv, badge_x + pad_left, icon_y);
        
        // Text
        int text_x = badge_x + pad_left + icon_w + pad_mid;
        int text_y = badge_y + (badge_h - text_height) / 2;
        ui_draw_text_to_buffer(cv->buf, cv->w, cv->h, text_x, text_y, s_simi_temperature_text, SIMI_RGB(255, 255, 255), text_scale);
        
        // Procedural Degree Symbol (°)
        char *space_ptr = strchr(s_simi_temperature_text, ' ');
        if (space_ptr) {
            int num_len = space_ptr - s_simi_temperature_text;
            char num_str[16] = {0};
            strncpy(num_str, s_simi_temperature_text, num_len);
            
            int num_width = ui_get_text_width(num_str, text_scale);
            int degree_x = text_x + num_width + 1;
            int degree_y = text_y;
            
            // Draw a tiny 3x3 hollow box
            canvas_fill_rect(cv, degree_x, degree_y, 3, 1, SIMI_RGB(255, 255, 255));
            canvas_fill_rect(cv, degree_x, degree_y + 2, 3, 1, SIMI_RGB(255, 255, 255));
            canvas_fill_rect(cv, degree_x, degree_y, 1, 3, SIMI_RGB(255, 255, 255));
            canvas_fill_rect(cv, degree_x + 2, degree_y, 1, 3, SIMI_RGB(255, 255, 255));
        }
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
