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

/* ── Geometría del lienzo (retrato centrado en la pantalla 320×240) ── */
#define SIMI_CANVAS_W 220
#define SIMI_CANVAS_H 210
#define SIMI_BLIT_X ((BSP_LCD_H_RES - SIMI_CANVAS_W) / 2) /* 50 */
#define SIMI_BLIT_Y ((BSP_LCD_V_RES - SIMI_CANVAS_H) / 2) /* 15 */
#define SIMI_CX (SIMI_CANVAS_W / 2)                       /* 110 */

#define SIMI_ANIM_STACK_SIZE 4096
#define SIMI_ANIM_PRIORITY (tskIDLE_PRIORITY + 1)
#define SIMI_ANIM_TRY_LOCK_MS 5
#define SIMI_ANIM_STOP_WAIT_MS 2600
#define SIMI_IDLE_WAKE_MS 500
#define SIMI_IDLE_BLINK_MIN_MS 4200
#define SIMI_IDLE_BLINK_JITTER_MS 1800

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
    default:
        break;
    }
}

static bool simi_blit_frame(bool blocking)
{
    if (!s_simi_screen_cleared)
    {
        ui_clear_screen();
        s_simi_screen_cleared = true;
        blocking = true;
    }

    bool ok = true;
    if (blocking)
    {
        ui_panel_blit(SIMI_BLIT_X, SIMI_BLIT_Y,
                      SIMI_BLIT_X + SIMI_CANVAS_W, SIMI_BLIT_Y + SIMI_CANVAS_H,
                      s_cv.buf);
    }
    else
    {
        ok = ui_panel_try_blit(SIMI_BLIT_X, SIMI_BLIT_Y,
                               SIMI_BLIT_X + SIMI_CANVAS_W, SIMI_BLIT_Y + SIMI_CANVAS_H,
                               s_cv.buf, SIMI_ANIM_TRY_LOCK_MS);
    }

    if (ok && !s_simi_backlight_ready)
    {
        ui_backlight_on();
        s_simi_backlight_ready = true;
    }
    return ok;
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
        bool blocking_blit = state_changed;

        wait_ms = SIMI_IDLE_WAKE_MS;

        if (visual_state == SIMI_STATE_TALKING)
        {
            render = true;
            wait_ms = 125;
        }
        else if (visual_state == SIMI_STATE_THINKING)
        {
            render = true;
            wait_ms = 250;
        }
        else if (visual_state == SIMI_STATE_ALERT)
        {
            render = true;
            wait_ms = 180;
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
                wait_ms = 80;
            }
            else if (next_blink_ms > now_ms)
            {
                uint32_t until_blink_ms = next_blink_ms - now_ms;
                wait_ms = until_blink_ms < SIMI_IDLE_WAKE_MS ? until_blink_ms : SIMI_IDLE_WAKE_MS;
            }
        }

        if (render)
        {
            simi_face_t f;
            simi_face_for_state(visual_state, &f);
            simi_apply_animation(visual_state, frame, speaking, blink_active, blink_frame, &f);
            simi_render(&f);

            bool blit_ok = simi_blit_frame(blocking_blit);
            if (blit_ok || blocking_blit)
            {
                last_visual_state = visual_state;
                last_speaking = speaking;
            }

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
    ESP_LOGW(TAG, "[HEAP] simi_canvas:before | INTERNAL free=%u largest=%u | PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
        // UI opcional: no caer a RAM interna, porque BLE necesita esa memoria.
        s_cv.buf = NULL;
    }

    if (!s_cv.buf)
    {
        ESP_LOGE(TAG, "Fallo al asignar el canvas de Dr. Simi en PSRAM (%u B)", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }

    s_cv.w = SIMI_CANVAS_W;
    s_cv.h = SIMI_CANVAS_H;
    s_simi_screen_cleared = false;
    s_simi_backlight_ready = false;

    // Diagnóstico: confirma DÓNDE quedó el lienzo y cuánta RAM interna costó.
    // Si 'ext_ram=1' el lienzo está en PSRAM y NO toca la RAM interna del BLE.
    ESP_LOGW(TAG, "[HEAP] simi_canvas:after  | ptr=%p ext_ram=%d | INTERNAL free=%u largest=%u | PSRAM free=%u",
             s_cv.buf, esp_ptr_external_ram(s_cv.buf) ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(simi_anim_task,
                                                    "simi_anim",
                                                    SIMI_ANIM_STACK_SIZE,
                                                    NULL,
                                                    SIMI_ANIM_PRIORITY,
                                                    &new_task,
                                                    tskNO_AFFINITY,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t ok = xTaskCreate(simi_anim_task,
                                "simi_anim",
                                SIMI_ANIM_STACK_SIZE,
                                NULL,
                                SIMI_ANIM_PRIORITY,
                                &new_task);
#endif
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
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
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

    if (task != NULL)
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
    if (xSemaphoreTake(s_anim_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        s_anim_speaking = active;
        task = s_anim_task;
        xSemaphoreGive(s_anim_mutex);
    }

    if (task != NULL)
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
        f->mouth_curve = 0; // boca recta (callado)
        f->brow_angle = -15;
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
    case SIMI_STATE_SLEEP:
        f->eye_open = 0; // ojos cerrados
        f->mouth_curve = 15;
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
    // Cuello ancho y corto para que la cabeza caiga sobre un cuerpo mas robusto.
    canvas_fill_rect(cv, SIMI_CX - 23, 147, 46, 34, C_SKIN);
    canvas_fill_ellipse(cv, SIMI_CX, 151, 30, 13, C_SKIN_SH);

    // Hombros y panza redondeada: Dr. Simi debe verse chaparrito/gordito,
    // no como una bata triangular delgada.
    canvas_fill_ellipse(cv, SIMI_CX - 70, 195, 38, 32, C_COAT_SH);
    canvas_fill_ellipse(cv, SIMI_CX + 70, 195, 38, 32, C_COAT_SH);
    canvas_fill_ellipse(cv, SIMI_CX, 208, 92, 50, C_COAT_SH);
    canvas_fill_ellipse(cv, SIMI_CX - 66, 191, 34, 30, C_COAT);
    canvas_fill_ellipse(cv, SIMI_CX + 66, 191, 34, 30, C_COAT);
    canvas_fill_ellipse(cv, SIMI_CX, 202, 86, 46, C_COAT);
    canvas_fill_round_rect(cv, 34, 166, SIMI_CANVAS_W - 68, 62, 40, C_COAT);

    // Camisa mas compacta; deja dominar el volumen blanco de la bata.
    canvas_fill_triangle(cv, SIMI_CX, 206, SIMI_CX - 29, 166, SIMI_CX + 29, 166, C_SHIRT);

    // Corbata azul oscura, un poco mas corta para no adelgazar visualmente el torso.
    canvas_fill_triangle(cv, SIMI_CX, 187, SIMI_CX - 9, 175, SIMI_CX + 9, 175, C_TIE);
    canvas_fill_triangle(cv, SIMI_CX - 7, 185, SIMI_CX + 7, 185, SIMI_CX, 205, C_TIE);

    // Cruz medica roja en la solapa izquierda.
    canvas_fill_rect(cv, 52 - 2, 194 - 7, 4, 16, C_CROSS);
    canvas_fill_rect(cv, 52 - 7, 194 - 2, 16, 4, C_CROSS);
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

/**
 * @brief Renders the full face described by @p f into the module canvas.
 */
static void simi_render(const simi_face_t *f)
{
    simi_canvas_t *cv = &s_cv;
    const int dy = f->head_dy;
    const bool alert_face = f->alert_border && f->brow_angle >= 80;

    canvas_clear(cv, f->bg);

    // Cuerpo primero (queda detrás del mentón)
    draw_body(cv);

    // ── Cabeza ──
    // Piel completa (es calvo)
    canvas_fill_ellipse(cv, SIMI_CX, 96 + dy, 66, 72, C_SKIN); // Forma base de la cabeza
    
    // Orejas
    canvas_fill_circle(cv, SIMI_CX - 66, 102 + dy, 14, C_SKIN);
    canvas_fill_circle(cv, SIMI_CX + 66, 102 + dy, 14, C_SKIN);

    // ── Los 3 cabellos a cada lado (más gruesos, blancos y como patitas de araña) ──
    // Izquierda (salen hacia arriba/afuera y luego caen)
    canvas_thick_line(cv, SIMI_CX - 62, 66 + dy, SIMI_CX - 76, 60 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX - 76, 60 + dy, SIMI_CX - 88, 68 + dy, 4, C_HAIR);

    canvas_thick_line(cv, SIMI_CX - 64, 80 + dy, SIMI_CX - 78, 76 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX - 78, 76 + dy, SIMI_CX - 90, 84 + dy, 4, C_HAIR);

    canvas_thick_line(cv, SIMI_CX - 64, 94 + dy, SIMI_CX - 76, 92 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX - 76, 92 + dy, SIMI_CX - 86, 102 + dy, 4, C_HAIR);

    // Derecha (salen hacia arriba/afuera y luego caen)
    canvas_thick_line(cv, SIMI_CX + 62, 66 + dy, SIMI_CX + 76, 60 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX + 76, 60 + dy, SIMI_CX + 88, 68 + dy, 4, C_HAIR);

    canvas_thick_line(cv, SIMI_CX + 64, 80 + dy, SIMI_CX + 78, 76 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX + 78, 76 + dy, SIMI_CX + 90, 84 + dy, 4, C_HAIR);

    canvas_thick_line(cv, SIMI_CX + 64, 94 + dy, SIMI_CX + 76, 92 + dy, 4, C_HAIR);
    canvas_thick_line(cv, SIMI_CX + 76, 92 + dy, SIMI_CX + 86, 102 + dy, 4, C_HAIR);

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
