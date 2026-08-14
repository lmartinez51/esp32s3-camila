/**
 * @file ui.c
 * @brief Legacy UI module stub. LVGL UI engine is implemented in camila_lvgl_ui.c.
 */

#include "ui.h"

// All active UI operations are handled by LVGL in camila_lvgl_ui.c
#if !USE_LVGL_UI

#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "UI";

static void clear_screen(void);
static void ui_panel_lock(void);
static void ui_panel_unlock(void);

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CHAR_SPACING_SCALE_1X 1
#define CHAR_SPACING_SCALE_2X 2
#define CHAR_SPACING_SCALE_3X 3

static esp_lcd_panel_handle_t g_panel_handle = NULL;
static esp_lcd_panel_io_handle_t g_io_handle = NULL;
static SemaphoreHandle_t s_panel_mutex = NULL;
static SemaphoreHandle_t s_panel_flush_done = NULL;
static int s_backlight_percent = -1;

static int g_status_msg_x = 0;
static int g_status_msg_y = 0;
static int g_status_msg_w = 0;
static int g_status_msg_h = 0;

static int g_help_msg_x = 0;
static int g_help_msg_y = 0;
static int g_help_msg_w = 0;
static int g_help_msg_h = 0;

/**
 * @brief Custom 8x8 pixel font definition for LCD display.
 *        Each character is represented as an array of 8 bytes, with each byte representing
 *        one row of 8 pixels. Bit value 1 = pixel on, 0 = pixel off.
 *        Font includes uppercase, lowercase letters, numbers, and special characters
 *        needed for the chatbot interface.
 */
static const uint8_t font_8x8[][8] = {
    {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, // 0 'A'
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, // 1 'I'
    {0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 2 ''' (apostrophe)
    {0x00, 0x00, 0x6C, 0x7E, 0x56, 0x46, 0x46, 0x00}, // 3 'm'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 4 ' ' (space)
    {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, // 5 'C'
    {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00}, // 6 'a'
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00}, // 7 'l'
    {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00}, // 8 'i'
    {0x00, 0x00, 0x3C, 0x60, 0x38, 0x06, 0x7C, 0x00}, // 9 's'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 10 ' ' (space duplicate)
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00}, // 11 'o'
    {0x00, 0x00, 0x5C, 0x66, 0x66, 0x66, 0x66, 0x00}, // 12 'n'
    {0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00}, // 13 '!' (exclamation mark)
    {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00}, // 14 'e'
    {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}, // 15 'E'
    {0x10, 0x10, 0x7C, 0x10, 0x10, 0x12, 0x0C, 0x00}, // 16 't'
    {0x00, 0x00, 0x5C, 0x66, 0x40, 0x40, 0x40, 0x00}, // 17 'r'
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, // 18 'W'
    {0x7E, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x00}, // 19 'F'
    {0x00, 0x00, 0x3C, 0x60, 0x60, 0x60, 0x3C, 0x00}, // 20 'c'
    {0x06, 0x06, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x00}, // 21 'd'
    {0x00, 0x00, 0x63, 0x63, 0x6B, 0x7F, 0x36, 0x00}, // 22 'w'
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, // 23 'T' (uppercase)
    {0x00, 0x00, 0x66, 0x66, 0x3E, 0x06, 0x3C, 0x00}, // 24 'y' (with descender)
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x3C, 0x46, 0x3C}, // 25 'g' (with descender)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00}, // 26 '.' (period/dot)
    {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, // 27 'D' (mayúscula)
    {0x0E, 0x18, 0x18, 0x3C, 0x18, 0x18, 0x18, 0x00}, // 28 'f' (minúscula)
    {0x66, 0x6C, 0x78, 0x60, 0x78, 0x6C, 0x66, 0x00}, // 29 'K'
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, // 30 'O'
    {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, // 31 'P'
    {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00}, // 32 'R'
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, // Índice 33: 'v' minúscula
    {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60}, // Índice 34: 'p' minúscula (con descender)
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, // Índice 35: 'h' minúscula
    {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}, // Índice 36: 'b' minúscula
    {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00}, // Índice 37: 'k' minúscula
    {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}, // Índice 38: 'S' mayúscula
    {0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x66, 0x00}, // Índice 39: 'N' mayúscula
    {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00}, // Índice 40: 'u' minúscula
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, // 41 'M' mayúscula corregida
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, // 42 'G' mayúscula corregida
    {0x00, 0x00, 0x3E, 0x60, 0x7C, 0x66, 0x3E, 0x00}, // 43 'S' mayúscula corregida
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, // Índice 44: 'U' mayúscula
    {0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x60}, // Índice 45: ':' (dos puntos)
    {0xCC, 0xCC, 0xCC, 0x78, 0x78, 0x30, 0x30, 0x00}, // Índice 46: 'V' mayúscula
    {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, // 47: '0'
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, // 48: '1'
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00}, // 49: '2'
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, // 50: '3'
    {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00}, // 51: '4'
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, // 52: '5'
    {0x3C, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, // 53: '6'
    {0x7E, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, // 54: '7'
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, // 55: '8'
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x3C, 0x00}, // 56: '9'
    {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}, // 57 'x' (minúscula)
    {0xC3, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x66, 0xC3}, // 58 'X' (mayúscula)
    {0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x00}, // 59 'Y' (mayúscula - alineada arriba)
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x06}, // 60 'q' (minúscula)
    {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0C, 0x00}, // 61 'Q' (mayúscula)
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, // 62 '?'
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, // 63 'L'
    {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00}, // 64 'z'
    {0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00}, // 65 '-'
    {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x00}, // 66 '/'
    {0x00, 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00}, // 67 '\'
    {0x00, 0x1C, 0x30, 0x60, 0x60, 0x60, 0x30, 0x1C}, // 68 '('
    {0x00, 0x38, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x38}, // 69 ')'
    {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, // 70 'B' (mayúscula)
};

/**
 * @brief Convierte un string de texto a un mapa de índices de la fuente.
 * @param str El string de entrada (solo soporta caracteres definidos en la fuente).
 * @param map_buffer El buffer de salida para los índices.
 * @param max_len El tamaño máximo del buffer.
 * @return El número de caracteres convertidos.
 */
static int convert_string_to_char_map(const char *str, int *map_buffer, int max_len)
{
    int count = 0;
    for (int i = 0; str[i] != '\0' && i < max_len; i++)
    {
        switch (str[i])
        {
        /* ── Dígitos ── */
        case '0': map_buffer[count++] = 47; break;
        case '1': map_buffer[count++] = 48; break;
        case '2': map_buffer[count++] = 49; break;
        case '3': map_buffer[count++] = 50; break;
        case '4': map_buffer[count++] = 51; break;
        case '5': map_buffer[count++] = 52; break;
        case '6': map_buffer[count++] = 53; break;
        case '7': map_buffer[count++] = 54; break;
        case '8': map_buffer[count++] = 55; break;
        case '9': map_buffer[count++] = 56; break;
        /* ── Mayúsculas ── */
        case 'A': map_buffer[count++] = 0;  break;
        case 'B': map_buffer[count++] = 70; break;
        case 'C': map_buffer[count++] = 5;  break;  // Antes faltaba
        case 'D': map_buffer[count++] = 27; break;
        case 'E': map_buffer[count++] = 15; break;  // Antes faltaba
        case 'F': map_buffer[count++] = 19; break;
        case 'G': map_buffer[count++] = 42; break;
        case 'I': map_buffer[count++] = 1;  break;  // Antes faltaba
        case 'K': map_buffer[count++] = 29; break;
        case 'L': map_buffer[count++] = 63; break;
        case 'M': map_buffer[count++] = 41; break;
        case 'N': map_buffer[count++] = 39; break;  // Antes faltaba
        case 'O': map_buffer[count++] = 30; break;
        case 'P': map_buffer[count++] = 31; break;
        case 'Q': map_buffer[count++] = 61; break;  // CORREGIDO: era 33 (colisión con 'v')
        case 'R': map_buffer[count++] = 32; break;
        case 'S': map_buffer[count++] = 38; break;
        case 'T': map_buffer[count++] = 23; break;  // Antes faltaba
        case 'U': map_buffer[count++] = 44; break;
        case 'V': map_buffer[count++] = 46; break;  // Antes faltaba
        case 'W': map_buffer[count++] = 18; break;
        case 'X': map_buffer[count++] = 58; break;
        case 'Y': map_buffer[count++] = 59; break;
        /* ── Minúsculas ── */
        case 'a': map_buffer[count++] = 6;  break;
        case 'b': map_buffer[count++] = 36; break;
        case 'c': map_buffer[count++] = 20; break;
        case 'd': map_buffer[count++] = 21; break;
        case 'e': map_buffer[count++] = 14; break;
        case 'f': map_buffer[count++] = 28; break;
        case 'g': map_buffer[count++] = 25; break;
        case 'h': map_buffer[count++] = 35; break;
        case 'i': map_buffer[count++] = 8;  break;
        case 'k': map_buffer[count++] = 37; break;
        case 'l': map_buffer[count++] = 7;  break;
        case 'm': map_buffer[count++] = 3;  break;
        case 'n': map_buffer[count++] = 12; break;
        case 'o': map_buffer[count++] = 11; break;
        case 'p': map_buffer[count++] = 34; break;
        case 'q': map_buffer[count++] = 60; break;
        case 'r': map_buffer[count++] = 17; break;
        case 's': map_buffer[count++] = 9;  break;
        case 't': map_buffer[count++] = 16; break;
        case 'u': map_buffer[count++] = 40; break;
        case 'v': map_buffer[count++] = 33; break;
        case 'w': map_buffer[count++] = 22; break;
        case 'x': map_buffer[count++] = 57; break;
        case 'y': map_buffer[count++] = 24; break;
        case 'z': map_buffer[count++] = 64; break;
        /* ── Símbolos ── */
        case ' ': map_buffer[count++] = 4;  break;
        case '!': map_buffer[count++] = 13; break;
        case '.': map_buffer[count++] = 26; break;
        case ':': map_buffer[count++] = 45; break;  // Antes faltaba
        case '?': map_buffer[count++] = 62; break;
        case '-': map_buffer[count++] = 65; break;
        case '/': map_buffer[count++] = 66; break;
        case '\\': map_buffer[count++] = 67; break;
        case '(': map_buffer[count++] = 68; break;
        case ')': map_buffer[count++] = 69; break;
        default:  break; // Ignora caracteres no soportados
        }
    }
    return count;
}

static bool IRAM_ATTR ui_panel_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                                   esp_lcd_panel_io_event_data_t *edata,
                                                   void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    BaseType_t high_task_woken = pdFALSE;
    SemaphoreHandle_t done = (SemaphoreHandle_t)user_ctx;
    if (done != NULL)
    {
        xSemaphoreGiveFromISR(done, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t ui_register_panel_callbacks(void)
{
    if (g_io_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_panel_flush_done == NULL)
    {
        s_panel_flush_done = xSemaphoreCreateBinary();
        if (s_panel_flush_done == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = ui_panel_color_trans_done_cb,
    };
    return esp_lcd_panel_io_register_event_callbacks(g_io_handle, &cbs, s_panel_flush_done);
}

static void ui_backlight_set_if_changed(int brightness_percent);

/**
 * @brief Sanitizes a text string by removing or replacing unsupported UTF-8 characters.
 *        Specifically removes inverted exclamation and question marks, and replaces
 *        accented characters with their unaccented equivalents.
 * @param text The input text string to sanitize (modified in place).
 */
void ui_sanitize_text(char *text)
{
    unsigned char *p = (unsigned char *)text;
    while (*p)
    {
        if (*p == 0xC2)
        {
            // Caracteres tipo ¡, ¿
            unsigned char next = *(p + 1);
            if (next == 0xA1 || next == 0xBF)
            {
                // ¡ (0xC2A1) o ¿ (0xC2BF) → eliminar
                memmove(p, p + 2, strlen((char *)(p + 2)) + 1);
                continue;
            }
        }
        else if (*p == 0xC3)
        {
            unsigned char next = *(p + 1);
            char replacement = 0;

            switch (next)
            {
            // Minúsculas con tilde
            case 0xA1:
                replacement = 'a';
                break; // á
            case 0xA9:
                replacement = 'e';
                break; // é
            case 0xAD:
                replacement = 'i';
                break; // í
            case 0xB3:
                replacement = 'o';
                break; // ó
            case 0xBA:
                replacement = 'u';
                break; // ú
            case 0xBC:
                replacement = 'u';
                break; // ü
            case 0xB1:
                replacement = 'n';
                break; // ñ

            // Mayúsculas con tilde
            case 0x81:
                replacement = 'A';
                break; // Á
            case 0x89:
                replacement = 'E';
                break; // É
            case 0x8D:
                replacement = 'I';
                break; // Í
            case 0x93:
                replacement = 'O';
                break; // Ó
            case 0x9A:
                replacement = 'U';
                break; // Ú
            case 0x9C:
                replacement = 'U';
                break; // Ü
            case 0x91:
                replacement = 'N';
                break; // Ñ
            default:
                break;
            }

            if (replacement)
            {
                *p = replacement;
                memmove(p + 1, p + 2, strlen((char *)(p + 2)) + 1);
                continue;
            }
        }

        p++;
    }
}



/**
 * @brief Initializes the UI system and LCD panel.
 *        Sets up the LCD display configuration, initializes brightness control,
 *        turns on the panel, sets default brightness.
 * @return ESP_OK on successful initialization, error code otherwise.
 */
esp_err_t ui_init(void)
{
    // Configuración del display con buffer de transferencia optimizado
    // Crear el mutex de panel antes de cualquier dibujo concurrente.
    if (s_panel_mutex == NULL)
    {
        s_panel_mutex = xSemaphoreCreateMutex();
        if (s_panel_mutex == NULL)
        {
            ESP_LOGE(TAG, "No se pudo crear el mutex del panel LCD");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_panel_handle != NULL && g_io_handle != NULL)
    {
        esp_lcd_panel_disp_on_off(g_panel_handle, true);
        return ESP_OK;
    }

    const bsp_display_config_t disp_cfg = {.max_transfer_sz = BSP_LCD_H_RES * 100 * sizeof(uint16_t)};

    esp_err_t err = bsp_display_new(&disp_cfg, &g_panel_handle, &g_io_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo inicializar el panel LCD: %s", esp_err_to_name(err));
        g_panel_handle = NULL;
        g_io_handle = NULL;
        return err;
    }

    err = ui_register_panel_callbacks();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo registrar callback de flush LCD: %s", esp_err_to_name(err));
        esp_lcd_panel_del(g_panel_handle);
        esp_lcd_panel_io_del(g_io_handle);
        spi_bus_free(BSP_LCD_SPI_NUM);
        g_panel_handle = NULL;
        g_io_handle = NULL;
        return err;
    }

    s_backlight_percent = -1;
    ui_backlight_set_if_changed(0);
    esp_lcd_panel_disp_on_off(g_panel_handle, true);
    clear_screen();
    return ESP_OK;
}

bool ui_is_initialized(void)
{
    return g_panel_handle != NULL && g_io_handle != NULL;
}

static esp_err_t ui_deinit_internal(bool keep_last_frame)
{
    if (!ui_is_initialized())
    {
        return ESP_OK;
    }

    if (!keep_last_frame)
    {
        ui_backlight_set_if_changed(0);
    }

    esp_err_t ret = ESP_OK;
    ui_panel_lock();

    if (g_panel_handle != NULL)
    {
        if (!keep_last_frame)
        {
            esp_err_t err = esp_lcd_panel_disp_on_off(g_panel_handle, false);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "No se pudo apagar el panel LCD: %s", esp_err_to_name(err));
                ret = err;
            }
        }

        esp_err_t err = esp_lcd_panel_del(g_panel_handle);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "No se pudo liberar el panel LCD: %s", esp_err_to_name(err));
            ret = err;
        }
        g_panel_handle = NULL;
    }

    if (g_io_handle != NULL)
    {
        esp_err_t err = esp_lcd_panel_io_del(g_io_handle);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "No se pudo liberar el IO SPI del LCD: %s", esp_err_to_name(err));
            ret = err;
        }
        g_io_handle = NULL;
    }

    esp_err_t bus_err = spi_bus_free(BSP_LCD_SPI_NUM);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "No se pudo liberar el bus SPI del LCD: %s", esp_err_to_name(bus_err));
        ret = bus_err;
    }

    g_status_msg_x = 0;
    g_status_msg_y = 0;
    g_status_msg_w = 0;
    g_status_msg_h = 0;
    g_help_msg_x = 0;
    g_help_msg_y = 0;
    g_help_msg_w = 0;
    g_help_msg_h = 0;

    ui_panel_unlock();
    ESP_LOGI(TAG, "LCD panel and SPI bus released%s",
             keep_last_frame ? " (last frame requested)" : "");
    return ret;
}

esp_err_t ui_deinit(void)
{
    return ui_deinit_internal(false);
}

esp_err_t ui_deinit_keep_last_frame(void)
{
    return ui_deinit_internal(true);
}

/**
 * @brief Toma el mutex del panel LCD. Bloqueante.
 *        Permite a otros módulos (p.ej. Dr. Simi) agrupar varios blits atómicos.
 */
void ui_panel_lock(void)
{
    if (s_panel_mutex)
    {
        xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    }
}

/**
 * @brief Libera el mutex del panel LCD.
 */
void ui_panel_unlock(void)
{
    if (s_panel_mutex)
    {
        xSemaphoreGive(s_panel_mutex);
    }
}

static DRAM_ATTR uint16_t s_blit_dma_buf[BSP_LCD_H_RES * 5];

static bool ui_panel_blit_internal(int x0, int y0, int x1, int y1,
                                   const void *pixels,
                                   TickType_t lock_wait_ticks,
                                   TickType_t flush_wait_ticks,
                                   bool log_failures)
{
    if (!g_panel_handle || !pixels)
    {
        return false;
    }
    if (x1 <= x0 || y1 <= y0)
    {
        return false;
    }
    if (!s_panel_mutex)
    {
        return false;
    }

    if (xSemaphoreTake(s_panel_mutex, lock_wait_ticks) != pdTRUE)
    {
        return false;
    }

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > BSP_LCD_H_RES) x1 = BSP_LCD_H_RES;
    if (y1 > BSP_LCD_V_RES) y1 = BSP_LCD_V_RES;
    if (x0 >= x1 || y0 >= y1)
    {
        xSemaphoreGive(s_panel_mutex);
        return false;
    }

    int w = x1 - x0;
    int max_rows = sizeof(s_blit_dma_buf) / (w * sizeof(uint16_t));
    if (max_rows < 1) max_rows = 1;
    if (max_rows > 5) max_rows = 5;

    bool ok = true;
    const uint8_t *p = (const uint8_t *)pixels;
    for (int y = y0; y < y1; y += max_rows) {
        int end_y = y + max_rows;
        if (end_y > y1) end_y = y1;
        int chunk_rows = end_y - y;
        size_t chunk_bytes = chunk_rows * w * sizeof(uint16_t);

        memcpy(s_blit_dma_buf, p, chunk_bytes);

        if (s_panel_flush_done) {
            xSemaphoreTake(s_panel_flush_done, 0);
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel_handle, x0, y, x1, end_y, s_blit_dma_buf);
        if (err != ESP_OK) {
            if (log_failures) {
                ESP_LOGW(TAG, "LCD blit failed: %s", esp_err_to_name(err));
            }
            ok = false;
            break;
        } else if (s_panel_flush_done &&
                   xSemaphoreTake(s_panel_flush_done, flush_wait_ticks) != pdTRUE) {
            if (log_failures) {
                ESP_LOGW(TAG, "LCD blit timed out waiting for SPI flush");
            }
            ok = false;
            break;
        }

        p += chunk_bytes;
    }
    xSemaphoreGive(s_panel_mutex);
    return ok;
}

/**
 * @brief Envía un bitmap al panel de forma protegida por el mutex.
 *        Único punto de acceso a esp_lcd_panel_draw_bitmap dentro del módulo UI.
 * @param x0,y0 Esquina superior izquierda (inclusiva).
 * @param x1,y1 Esquina inferior derecha (exclusiva).
 * @param pixels Buffer de píxeles en formato del panel (16 bpp).
 */
void ui_panel_blit(int x0, int y0, int x1, int y1, const void *pixels)
{
    (void)ui_panel_blit_internal(x0, y0, x1, y1, pixels,
                                 portMAX_DELAY,
                                 pdMS_TO_TICKS(2000),
                                 true);
}

bool ui_panel_try_blit(int x0, int y0, int x1, int y1,
                       const void *pixels, uint32_t lock_timeout_ms)
{
    return ui_panel_blit_internal(x0, y0, x1, y1, pixels,
                                  pdMS_TO_TICKS(lock_timeout_ms),
                                  pdMS_TO_TICKS(120),
                                  false);
}

/**
 * @brief Clears the entire LCD screen to black.
 *        Fills the complete display area with black pixels (0x0000).
 */
/**
 * @brief Limpia la pantalla completa enviando bloques de 20 filas por transacción SPI.
 *        Reduce de 240 transacciones a ~12, acelerando el borrado ~20×.
 */
static void clear_screen(void)
{
#define CLEAR_CHUNK_LINES 20
    /* Buffer forzado a DRAM interna con DRAM_ATTR para garantizar que el SPI DMA
     * puede acceder directamente. Sin este atributo el linker puede colocarlo en
     * PSRAM (si CONFIG_SPIRAM_ALLOW_BSS_SEG=y), donde el DMA no puede operar
     * directamente y el driver queda bloqueado esperando un bounce-copy indefinido.
     * 20 x 320 x 2 = 12 800 bytes. */
    static DRAM_ATTR uint16_t clear_buf[CLEAR_CHUNK_LINES * BSP_LCD_H_RES];
    memset(clear_buf, 0x00, sizeof(clear_buf));

    int lines_sent = 0;
    while (lines_sent < BSP_LCD_V_RES)
    {
        int chunk = ((lines_sent + CLEAR_CHUNK_LINES) <= BSP_LCD_V_RES)
                        ? CLEAR_CHUNK_LINES
                        : (BSP_LCD_V_RES - lines_sent);
        /* ui_panel_try_blit con timeout de 2 s evita que el orchestrator
         * se bloquee indefinidamente si el bus SPI esta congestionado. */
        if (!ui_panel_try_blit(0, lines_sent,
                               BSP_LCD_H_RES, lines_sent + chunk,
                               clear_buf, 2000))
        {
            ESP_LOGW(TAG, "clear_screen: blit timeout at line %d -- aborting", lines_sent);
            break;
        }
        lines_sent += chunk;
    }
#undef CLEAR_CHUNK_LINES
}

/**
 * @brief Limpia toda la pantalla a negro (envoltorio público de clear_screen).
 */
void ui_clear_screen(void)
{
    clear_screen();
}

/**
 * @brief Draws a single character into a pixel buffer.
 *        Renders a character from the font array into the specified buffer location
 *        with the given color and scale factor.
 * @param target_buffer Target pixel buffer for character rendering.
 * @param buffer_width Width of the target buffer in pixels.
 * @param offset_x X position offset within the buffer.
 * @param offset_y Y position offset within the buffer.
 * @param char_index Index of the character in the font array.
 * @param color Character color in BGR565 format.
 * @param scale Scaling factor (1, 2, or 3).
 */
static void draw_char_to_buffer(uint16_t *target_buffer, int buffer_width, int buffer_height, int offset_x, int offset_y, int char_index, uint16_t color, int scale)
{
    for (int row = 0; row < CHAR_HEIGHT; row++)
    {
        uint8_t line = font_8x8[char_index][row];
        for (int col = 0; col < CHAR_WIDTH; col++)
        {
            uint16_t px_color = (line & (0x80 >> col)) ? color : 0x0000;
            if (px_color == 0x0000) continue; // transparent background

            for (int dy = 0; dy < scale; dy++)
            {
                for (int dx = 0; dx < scale; dx++)
                {
                    int final_px = offset_x + (col * scale) + dx;
                    int final_py = offset_y + (row * scale) + dy;
                    if (final_px >= 0 && final_px < buffer_width && final_py >= 0 && final_py < buffer_height)
                    {
                        target_buffer[final_py * buffer_width + final_px] = px_color;
                    }
                }
            }
        }
    }
}

/**
 * @brief Renders text string to the LCD display.
 *        Creates a buffer for the complete text string, renders all characters
 *        into it, and then displays the entire text as a single bitmap operation.
 * @param start_x Starting X position for the text.
 * @param start_y Starting Y position for the text.
 * @param char_map Array of character indices from the font array.
 * @param num_chars Number of characters in the char_map array.
 * @param color Text color in BGR565 format.
 * @param scale Text scaling factor (1, 2, or 3).
 */
/**
 * @brief Renderiza texto en el LCD usando un buffer estático fijo.
 *        Evita malloc/free por llamada (P-05): buffer estático de 320×24×2 = 15 360 bytes.
 *        Llamar solo desde una tarea a la vez (el buffer no es reentrante).
 */
static void display_text(int start_x, int start_y, const int *char_map, int num_chars, uint16_t color, int scale)
{
    if (!g_panel_handle || num_chars == 0)
        return;

    int display_char_width = CHAR_WIDTH * scale;
    int display_char_height = CHAR_HEIGHT * scale;

    // Determinar espaciado entre caracteres según la escala
    int char_spacing;
    if (scale == 3)
    {
        char_spacing = CHAR_SPACING_SCALE_3X;
    }
    else if (scale == 2)
    {
        char_spacing = CHAR_SPACING_SCALE_2X;
    }
    else
    {
        char_spacing = CHAR_SPACING_SCALE_1X;
    }

    // Calcular dimensiones totales del texto
    int total_width = num_chars * display_char_width + (num_chars - 1) * char_spacing;
    int total_height = display_char_height;

    // Asignar buffer para todo el texto en PSRAM para no agotar la memoria DMA interna
    uint16_t *full_buffer = heap_caps_malloc(total_width * total_height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!full_buffer)
    {
        full_buffer = malloc(total_width * total_height * sizeof(uint16_t));
    }
    if (!full_buffer)
    {
        ESP_LOGE(TAG, "Fallo al asignar memoria para el buffer de texto!");
        return;
    }

    // Inicializar buffer con fondo negro
    memset(full_buffer, 0x00, total_width * total_height * sizeof(uint16_t));

    // Dibujar cada carácter en el buffer
    for (int i = 0; i < num_chars; i++)
    {
        int char_offset_x = i * (display_char_width + char_spacing);
        draw_char_to_buffer(full_buffer, total_width, total_height, char_offset_x, 0, char_map[i], color, scale);
    }

    // Enviar el buffer completo al display
    ui_panel_blit(start_x, start_y, start_x + total_width, start_y + total_height, full_buffer);
    free(full_buffer);
}

void ui_draw_text_to_buffer(uint16_t *buffer, int buffer_w, int buffer_h,
                            int start_x, int start_y,
                            const char *text, uint16_t color, int scale)
{
    if (!buffer || !text) return;

    int char_map[32];
    int num_chars = convert_string_to_char_map(text, char_map, 32);
    if (num_chars == 0) return;

    int display_char_width = CHAR_WIDTH * scale;
    int char_spacing = (scale == 3) ? CHAR_SPACING_SCALE_3X : CHAR_SPACING_SCALE_2X;

    for (int i = 0; i < num_chars; i++) {
        int char_offset_x = start_x + i * (display_char_width + char_spacing);
        draw_char_to_buffer(buffer, buffer_w, buffer_h, char_offset_x, start_y, char_map[i], color, scale);
    }
}

static void draw_filled_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!g_panel_handle || width <= 0 || height <= 0)
        return;

    if (x >= BSP_LCD_H_RES || y >= BSP_LCD_V_RES)
        return;

    if (x + width  > BSP_LCD_H_RES) width  = BSP_LCD_H_RES - x;
    if (y + height > BSP_LCD_V_RES) height = BSP_LCD_V_RES - y;

    int total_pixels = width * height;
    uint16_t *buf = heap_caps_malloc(total_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
    {
        buf = malloc(total_pixels * sizeof(uint16_t));
    }
    if (!buf)
        return;

    for (int i = 0; i < total_pixels; i++)
    {
        buf[i] = color;
    }

    ui_panel_blit(x, y, x + width, y + height, buf);
    free(buf);
}

static void draw_screen_border(uint16_t color, int thickness)
{
    const int margin_x = 8;
    const int margin_y = 6;

    int x0 = margin_x;
    int y0 = margin_y;
    int x1 = BSP_LCD_H_RES - margin_x;
    int y1 = BSP_LCD_V_RES - margin_y;

    draw_filled_rect(x0, y0, x1 - x0, thickness, color);
    draw_filled_rect(x0, y1 - thickness, x1 - x0, thickness, color);
    draw_filled_rect(x0, y0, thickness, y1 - y0, color);
    draw_filled_rect(x1 - thickness, y0, thickness, y1 - y0, color);
}

/* Mute countdown persistent overlay — survives display_system_phase_message repaints */
/* NOTE: declared here (before display_system_phase_message) so the function can access them */
static bool s_mute_overlay_active = false;
static char s_mute_overlay_text[32] = {0};

/**
 * @brief Draws the mute countdown band directly to LCD in 1 atomic, non-blocking pass.
 * Uses ui_panel_try_blit (50ms max timeout) so it NEVER blocks the FreeRTOS timer thread.
 */
static void ui_draw_mute_countdown_band(void)
{
    if (!s_mute_overlay_active || s_mute_overlay_text[0] == '\0') return;

    int text_map[32];
    int num_chars = convert_string_to_char_map(s_mute_overlay_text, text_map, 32);
    if (num_chars <= 0) return;

    const int band_w = 296;
    const int band_h = 35;
    const int band_x = 12;
    const int band_y = 190;

    int total_pixels = band_w * band_h;
    uint16_t *band_buf = heap_caps_malloc(total_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!band_buf)
    {
        band_buf = malloc(total_pixels * sizeof(uint16_t));
    }
    if (!band_buf) return;

    // Clear entire 296x35 band to black
    memset(band_buf, 0x00, total_pixels * sizeof(uint16_t));

    int scale = 2;
    int char_spacing = CHAR_SPACING_SCALE_2X;
    int text_w = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * char_spacing;
    if (text_w > band_w)
    {
        scale = 1;
        char_spacing = CHAR_SPACING_SCALE_1X;
        text_w = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * char_spacing;
    }
    int rel_x = (band_w - text_w) / 2;
    if (rel_x < 0) rel_x = 0;
    int rel_y = (scale == 2) ? 5 : 10;

    // Render font glyphs into composite band buffer
    for (int i = 0; i < num_chars; i++)
    {
        int char_offset_x = rel_x + i * (CHAR_WIDTH * scale + char_spacing);
        draw_char_to_buffer(band_buf, band_w, band_h, char_offset_x, rel_y, text_map[i], COLOR_YELLOW_BGR565, scale);
    }

    // Single non-blocking SPI blit (50ms timeout)
    ui_panel_try_blit(band_x, band_y, band_x + band_w, band_y + band_h, band_buf, 50);

    free(band_buf);
}

void display_system_phase_message(const char *title, const char *subtitle, uint16_t color)
{
    char title_buf[64] = {0};
    char sub_buf[128] = {0};

    if (title && title[0] != '\0') {
        strncpy(title_buf, title, sizeof(title_buf) - 1);
        ui_sanitize_text(title_buf);
    } else {
        strncpy(title_buf, "Camila AI", sizeof(title_buf) - 1);
    }

    if (subtitle && subtitle[0] != '\0') {
        strncpy(sub_buf, subtitle, sizeof(sub_buf) - 1);
        ui_sanitize_text(sub_buf);
    }

    char sub_line1[32] = {0};
    char sub_line2[32] = {0};
    bool has_sub2 = false;

    if (sub_buf[0] != '\0') {
        size_t sub_len = strlen(sub_buf);
        if (sub_len <= 14) {
            strncpy(sub_line1, sub_buf, sizeof(sub_line1) - 1);
        } else {
            int split_idx = 14;
            for (int i = 14; i >= 4; i--) {
                if (sub_buf[i] == ' ' || sub_buf[i] == '/') {
                    split_idx = i;
                    break;
                }
            }
            strncpy(sub_line1, sub_buf, split_idx);
            sub_line1[split_idx] = '\0';
            const char *rest = sub_buf + split_idx + (sub_buf[split_idx] == ' ' ? 1 : 0);
            strncpy(sub_line2, rest, sizeof(sub_line2) - 1);
            if (sub_line2[0] != '\0') {
                has_sub2 = true;
            }
        }
    }

    int title_map[32];
    int sub1_map[32];
    int sub2_map[32];

    int title_chars = convert_string_to_char_map(title_buf, title_map, 32);
    int sub1_chars = sub_line1[0] != '\0' ? convert_string_to_char_map(sub_line1, sub1_map, 32) : 0;
    int sub2_chars = has_sub2 ? convert_string_to_char_map(sub_line2, sub2_map, 32) : 0;

    if (title_chars <= 0 && sub1_chars <= 0 && sub2_chars <= 0)
    {
        ESP_LOGW(TAG, "System phase message skipped: no supported glyphs");
        return;
    }

    int title_scale = (title_chars > 14) ? 1 : 2;
    int sub1_scale = (sub1_chars > 14) ? 1 : 2;
    int sub2_scale = (sub2_chars > 14) ? 1 : 2;

    int title_spacing = (title_scale == 2) ? CHAR_SPACING_SCALE_2X : CHAR_SPACING_SCALE_1X;
    int sub1_spacing = (sub1_scale == 2) ? CHAR_SPACING_SCALE_2X : CHAR_SPACING_SCALE_1X;
    int sub2_spacing = (sub2_scale == 2) ? CHAR_SPACING_SCALE_2X : CHAR_SPACING_SCALE_1X;

    int title_h = CHAR_HEIGHT * title_scale;
    int sub1_h = sub1_chars > 0 ? (CHAR_HEIGHT * sub1_scale) : 0;
    int sub2_h = sub2_chars > 0 ? (CHAR_HEIGHT * sub2_scale) : 0;
    int line_gap = 12;

    int total_lines = (title_chars > 0 ? 1 : 0) + (sub1_chars > 0 ? 1 : 0) + (sub2_chars > 0 ? 1 : 0);
    int total_h = title_h + sub1_h + sub2_h + (total_lines - 1) * line_gap;
    int y = (BSP_LCD_V_RES - total_h) / 2;

    clear_screen();
    draw_screen_border(color, 2);

    if (title_chars > 0)
    {
        int title_w = title_chars * (CHAR_WIDTH * title_scale) + (title_chars - 1) * title_spacing;
        int x = (BSP_LCD_H_RES - title_w) / 2;
        if (x < 8) x = 8;
        display_text(x, y, title_map, title_chars, color, title_scale);
        y += title_h + line_gap;
    }

    if (sub1_chars > 0)
    {
        int sub1_w = sub1_chars * (CHAR_WIDTH * sub1_scale) + (sub1_chars - 1) * sub1_spacing;
        int x = (BSP_LCD_H_RES - sub1_w) / 2;
        if (x < 8) x = 8;
        display_text(x, y, sub1_map, sub1_chars, COLOR_WHITE_BGR565, sub1_scale);
        y += sub1_h + line_gap;
    }

    if (sub2_chars > 0)
    {
        int sub2_w = sub2_chars * (CHAR_WIDTH * sub2_scale) + (sub2_chars - 1) * sub2_spacing;
        int x = (BSP_LCD_H_RES - sub2_w) / 2;
        if (x < 8) x = 8;
        display_text(x, y, sub2_map, sub2_chars, COLOR_WHITE_BGR565, sub2_scale);
    }

    ui_backlight_on();

    if (s_mute_overlay_active && s_mute_overlay_text[0] != '\0')
    {
        ui_draw_mute_countdown_band();
    }

    ESP_LOGI(TAG, "System phase displayed: %s / %s",
             title ? title : "", subtitle ? subtitle : "");
}

void display_startup_screen(void)
{
    display_system_phase_message("Welcome!", "Starting up", COLOR_CYAN_BGR565);
}

void display_welcome_identity(const char *name)
{
    const char *identity_name = (name && name[0] != '\0') ? name : "Lorenzo";
    char buf[64];
    snprintf(buf, sizeof(buf), "Spill it, %s...", identity_name);
    display_system_phase_message("Camila AI", buf, COLOR_WHITE_BGR565);
}

void display_error_message(void)
{
    display_system_phase_message("SYSTEM ERROR", "Connection Error / Retrying", COLOR_RED_BGR565);
}

void display_resetting_message(void)
{
    display_system_phase_message("REBOOTING", "Resetting system...", COLOR_YELLOW_BGR565);
}

void display_disconnected_message(void)
{
    display_system_phase_message("DISCONNECTED", "WiFi Disconnected", COLOR_RED_BGR565);
}

void display_network_timeout_message(void)
{
    display_system_phase_message("NETWORK TIMEOUT", "WiFi Signal Weak / Retrying...", COLOR_RED_BGR565);
}

void display_api_key_error_message(void)
{
    display_system_phase_message("API ERROR 429", "Quota Exceeded / Rate Limit", COLOR_RED_BGR565);
}

void display_intruder_alert_message(void)
{
    display_system_phase_message("CENTINELA", "Security Alert: Access Denied", COLOR_RED_BGR565);
}

void display_config_mode_message(void)
{
    display_system_phase_message("CONFIG MODE", "Provisioning Active...", COLOR_CYAN_BGR565);
}

void display_wifi_creds(void)
{
    display_system_phase_message("PROVISIONING", "Enter WiFi creds via BLE", COLOR_YELLOW_BGR565);
}

static char s_camila_overlay_text[64] = {0};
static uint16_t s_camila_overlay_color = COLOR_WHITE_BGR565;
static SemaphoreHandle_t s_camila_overlay_mutex = NULL;

void ui_show_status_message(const char *msg, uint16_t color)
{
    if (s_camila_overlay_mutex == NULL)
    {
        s_camila_overlay_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(s_camila_overlay_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        if (msg && msg[0] != '\0')
        {
            strncpy(s_camila_overlay_text, msg, sizeof(s_camila_overlay_text) - 1);
            s_camila_overlay_text[sizeof(s_camila_overlay_text) - 1] = '\0';
            s_camila_overlay_color = color;

            draw_filled_rect(12, 190, 296, 35, COLOR_BLACK_BGR565);
            int text_map[32];
            int num_chars = convert_string_to_char_map(s_camila_overlay_text, text_map, 32);
            if (num_chars > 0)
            {
                int scale = 2;
                int char_spacing = CHAR_SPACING_SCALE_2X;
                int text_w = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * char_spacing;
                if (text_w > 296)
                {
                    scale = 1;
                    char_spacing = CHAR_SPACING_SCALE_1X;
                    text_w = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * char_spacing;
                    if (text_w > 296)
                    {
                        num_chars = (296 + char_spacing) / (CHAR_WIDTH * scale + char_spacing);
                        if (num_chars < 1) num_chars = 1;
                        text_w = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * char_spacing;
                    }
                }
                int x = (BSP_LCD_H_RES - text_w) / 2;
                if (x < 12) x = 12;
                int y = (scale == 2) ? 195 : 200;
                display_text(x, y, text_map, num_chars, s_camila_overlay_color, scale);
            }
        }
        else
        {
            if (s_camila_overlay_text[0] != '\0')
            {
                s_camila_overlay_text[0] = '\0';
                draw_filled_rect(12, 190, 296, 35, COLOR_BLACK_BGR565);
            }
        }
        xSemaphoreGive(s_camila_overlay_mutex);
    }
}

void ui_clear_status_message(void)
{
    ui_show_status_message(NULL, 0);
}

void ui_show_help_message_below_status(const char *msg, uint16_t color)
{
    ui_show_status_message(msg, color);
}

void ui_clear_help_message_below_status(void)
{
    ui_clear_status_message();
}

void camila_ui_update_state_with_color(ui_state_t state, const char *title, const char *subtitle, uint16_t color)
{
    uint16_t render_color = color;
    if (render_color == 0) {
        switch (state) {
            case UI_STATE_BOOT:
                render_color = COLOR_MAGENTA_BGR565;
                break;
            case UI_STATE_WIFI_CONNECTING:
                render_color = COLOR_YELLOW_BGR565;
                break;
            case UI_STATE_BLE_SCAN:
            case UI_STATE_BLE_DISCOVERY:
                render_color = COLOR_CYAN_BGR565;
                break;
            case UI_STATE_SUCCESS:
                render_color = COLOR_GREEN_BGR565;
                break;
            case UI_STATE_ACTIVE_WEBRTC:
                render_color = COLOR_WHITE_BGR565;
                break;
            case UI_STATE_ALERT_VIGILANTE:
            case UI_STATE_ERROR:
                render_color = COLOR_RED_BGR565;
                break;
            default:
                render_color = COLOR_CYAN_BGR565;
                break;
        }
    }

    const char *title_str = title ? title : "Camila AI";
    const char *sub_str = subtitle ? subtitle : "Active";

    display_system_phase_message(title_str, sub_str, render_color);
}

void camila_ui_update_state(ui_state_t state, const char *title, const char *subtitle)
{
    camila_ui_update_state_with_color(state, title, subtitle, 0);
}

void camila_ui_show_avatar(void)
{
    camila_ui_update_state(UI_STATE_ACTIVE_WEBRTC, "CAMILA AI", "Ready and listening");
}

void camila_ui_set_speaking_state(bool is_speaking)
{
    (void)is_speaking;
}

void camila_ui_update_mute_countdown(int remaining_seconds)
{
    if (remaining_seconds < 0) return;
    int mins = remaining_seconds / 60;
    int secs = remaining_seconds % 60;
    /* Save text for display_system_phase_message to restore on full repaints */
    snprintf(s_mute_overlay_text, sizeof(s_mute_overlay_text), "Auto-sleep in %02d:%02d", mins, secs);
    s_mute_overlay_active = true;
    /* Direct draw — only s_panel_mutex, NO s_camila_overlay_mutex (avoids timer-task contention) */
    ui_draw_mute_countdown_band();
}

/**
 * @brief Clears the persistent mute countdown overlay.
 * Must be called when the device exits MUTE state so the overlay
 * is never restored by subsequent display_system_phase_message calls.
 * Unconditional clear: does NOT depend on s_camila_overlay_text state.
 */
void camila_ui_clear_mute_countdown(void)
{
    s_mute_overlay_active = false;
    s_mute_overlay_text[0] = '\0';
    /* Erase the countdown band non-blockingly (100ms timeout) */
    uint16_t *clear_buf = heap_caps_malloc(296 * 35 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (clear_buf)
    {
        memset(clear_buf, 0x00, 296 * 35 * sizeof(uint16_t));
        ui_panel_try_blit(12, 190, 12 + 296, 190 + 35, clear_buf, 100);
        free(clear_buf);
    }
}





static void ui_backlight_set_if_changed(int brightness_percent)
{
    if (brightness_percent < 0)
    {
        brightness_percent = 0;
    }
    else if (brightness_percent > 100)
    {
        brightness_percent = 100;
    }

    if (s_backlight_percent == brightness_percent)
    {
        return;
    }

    esp_err_t err = bsp_display_brightness_set(brightness_percent);
    if (err == ESP_OK)
    {
        s_backlight_percent = brightness_percent;
    }
    else
    {
        ESP_LOGW(TAG, "No se pudo ajustar backlight a %d%%: %s",
                 brightness_percent,
                 esp_err_to_name(err));
    }
}

/**
 * @brief Safely turns off the LCD backlight without affecting other systems.
 *        Uses gradual brightness reduction and only disables backlight, not the panel.
 *        This prevents interference with WiFi connectivity and other shared resources.
 */
void ui_backlight_off_safe(void)
{
    // Apagado gradual del brillo para evitar cambios bruscos en el sistema
    for (int brightness = 50; brightness >= 0; brightness -= 5)
    {
        ui_backlight_set_if_changed(brightness);
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms entre cada paso
    }

    // SOLO apagar el backlight, NO el panel LCD
    // Esto evita conflictos con recursos compartidos del SPI
    ui_backlight_set_if_changed(0);

    // NO llamar esp_lcd_panel_disp_on_off() aquí para evitar conflictos
    // ESP_LOGI(TAG, "LCD backlight turned off safely (panel remains active).");
}

/**
 * @brief Turns the LCD backlight back on.
 *        Restores the display brightness to working level.
 */
void ui_backlight_on(void)
{
    ui_backlight_set_if_changed(50); // Restaurar brillo al 50%
}



#endif // USE_LVGL_UI
