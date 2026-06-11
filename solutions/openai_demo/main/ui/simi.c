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

#include <string.h>

static const char *TAG = "SIMI";

/* ── Geometría del lienzo (retrato centrado en la pantalla 320×240) ── */
#define SIMI_CANVAS_W 220
#define SIMI_CANVAS_H 210
#define SIMI_BLIT_X ((BSP_LCD_H_RES - SIMI_CANVAS_W) / 2) /* 50 */
#define SIMI_BLIT_Y ((BSP_LCD_V_RES - SIMI_CANVAS_H) / 2) /* 15 */
#define SIMI_CX (SIMI_CANVAS_W / 2)                       /* 110 */

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

static simi_canvas_t s_cv = {0};
static bool s_simi_screen_cleared = false;

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

void ui_simi_deinit(void)
{
    if (s_cv.buf)
    {
        heap_caps_free(s_cv.buf);
        s_cv.buf = NULL;
    }
    s_simi_screen_cleared = false;
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
        f->mouth_curve = -70;  // ceño/boca hacia abajo
        f->brow_angle = 90;    // cejas muy enojadas
        f->eye_open = 100;
        f->alert_border = true;
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
    // Cuello (piel) que une el mentón con la bata
    canvas_fill_rect(cv, SIMI_CX - 20, 148, 40, 34, C_SKIN);
    canvas_fill_ellipse(cv, SIMI_CX, 150, 22, 12, C_SKIN_SH); // sombra bajo el mentón

    // Hombros / bata blanca con sombra de cuello para dar volumen
    canvas_fill_round_rect(cv, 26, 170, SIMI_CANVAS_W - 52, 60, 34, C_COAT_SH);
    canvas_fill_round_rect(cv, 30, 174, SIMI_CANVAS_W - 60, 56, 32, C_COAT);

    // Camisa azul en V (cuello)
    canvas_fill_triangle(cv, SIMI_CX, 210, SIMI_CX - 26, 166, SIMI_CX + 26, 166, C_SHIRT);

    // Corbata azul oscura
    canvas_fill_triangle(cv, SIMI_CX, 186, SIMI_CX - 8, 174, SIMI_CX + 8, 174, C_TIE);
    canvas_fill_triangle(cv, SIMI_CX - 6, 184, SIMI_CX + 6, 184, SIMI_CX, 210, C_TIE);

    // Cruz médica roja en la solapa izquierda
    canvas_fill_rect(cv, 58 - 2, 196 - 7, 4, 16, C_CROSS);
    canvas_fill_rect(cv, 58 - 7, 196 - 2, 16, 4, C_CROSS);
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
    int brow_arc = -6 - (f->brow_angle * 4 / 100); // curvar hacia arriba
    int browY = 66 + dy;
    canvas_smile(cv, SIMI_CX - 24, browY, 14, brow_arc, 5, C_HAIR);
    canvas_smile(cv, SIMI_CX + 24, browY, 14, brow_arc, 5, C_HAIR);

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
    int mo = 12 + (f->mouth_open * 10 / 100);
    
    // Cavidad de la boca (oscura)
    canvas_fill_ellipse(cv, SIMI_CX, mouthY, 20, mo, C_MOUTH_OPEN);
    
    // Lengua (elipse rosa/roja en la parte inferior)
    if (mo > 6) {
        canvas_fill_ellipse(cv, SIMI_CX, mouthY + mo - 4, 12, 6, C_TONGUE);
    }

    // ── Bigote blanco enorme (rasgo icónico, tapa parte de la boca) ──
    // Dos elipses centrales (más delgadas para no tapar tanto la boca)
    canvas_fill_ellipse(cv, SIMI_CX - 20, 128 + dy, 26, 14, C_HAIR);
    canvas_fill_ellipse(cv, SIMI_CX + 20, 128 + dy, 26, 14, C_HAIR);
    // Puntas redondeadas cayendo hacia los lados y un poco hacia abajo
    canvas_fill_ellipse(cv, SIMI_CX - 40, 132 + dy, 16, 12, C_HAIR);
    canvas_fill_ellipse(cv, SIMI_CX + 40, 132 + dy, 16, 12, C_HAIR);

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
    if (!ui_simi_ready())
    {
        ESP_LOGW(TAG, "ui_simi_render_static llamado sin canvas inicializado");
        return;
    }

    simi_face_t f;
    simi_face_for_state(state, &f);
    simi_render(&f);

    if (!s_simi_screen_cleared) {
        // Limpia restos de texto que quedan fuera del lienzo (p.ej. el splash previo).
        ui_clear_screen();
        s_simi_screen_cleared = true;
    }

    ui_panel_blit(SIMI_BLIT_X, SIMI_BLIT_Y,
                  SIMI_BLIT_X + SIMI_CANVAS_W, SIMI_BLIT_Y + SIMI_CANVAS_H,
                  s_cv.buf);
    ui_backlight_on();
}
