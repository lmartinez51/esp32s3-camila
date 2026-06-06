/**
 * @file ui.c
 * @brief User Interface implementation for the ESP32-S3-BOX3 AI Chatbot using OpenAI Realtime API.
 *        Provides LCD display management, text rendering with custom 8x8 font, and various UI states
 *        including startup screens, WiFi connection status, and error messages.
 *
 * @author Lorenzo Martínez
 * @date 2025
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

// Cabecera principal del módulo
#include "ui.h"

// Headers del sistema ESP-IDF
#include "esp_err.h"
#include "esp_log.h"

// Headers de la Board Support Package (BSP)
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

// Headers para control de LCD
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"

// Headers de la biblioteca estándar C
#include <string.h> // Para memset()
#include <stdlib.h> // Para malloc() y free()
#include <stdio.h>

static const char *TAG = "UI";
esp_lcd_panel_handle_t g_panel_handle = NULL;
esp_lcd_panel_io_handle_t g_io_handle = NULL;
// Variables para recordar la posición y tamaño del último mensaje de estado
static int g_status_msg_x = 0;
static int g_status_msg_y = 0;
static int g_status_msg_w = 0;
static int g_status_msg_h = 0;
// Variables globales para rastrear el área del mensaje de ayuda
static int g_help_msg_x = 0;
static int g_help_msg_y = 0;
static int g_help_msg_w = 0;
static int g_help_msg_h = 0;

// Definiciones de tamaño de caracteres y escala para el sistema de fuentes
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CHAR_SPACING_SCALE_1X 1 // Espaciado entre caracteres para escala 1x
#define CHAR_SPACING_SCALE_2X 4 // Espaciado entre caracteres para escala 2x
#define CHAR_SPACING_SCALE_3X 6 // Espaciado entre caracteres para escala 3x

// Prototipos de funciones privadas
static void display_text(int start_x, int start_y, const int *char_map, int num_chars, uint16_t color, int scale);
static void draw_char_to_buffer(uint16_t *target_buffer, int buffer_width, int offset_x, int offset_y, int char_index, uint16_t color, int scale);
static void clear_screen(void);
static void draw_filled_rect(int x, int y, int width, int height, uint16_t color);
static void draw_screen_border(uint16_t color, int thickness);
static void display_welcome_message(void);
static void display_camila_text(void);
static void display_drsimi_text(void);
static int convert_string_to_char_map(const char *str, int *map_buffer, int max_len);

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
    {0x00, 0x00, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x00}, // 59 'Y' (mayúscula)
    {0x00, 0x00, 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x06}, // 60 'q' (minúscula)
    {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0C, 0x00}, // 61 'Q' (mayúscula)
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, // 62 '?'
    // {0x06, 0x00, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, // 62 í (i con tilde)
    // {0x00, 0x18, 0x00, 0x18, 0x30, 0x60, 0x66, 0x3C}, // 63 '¿' (final, vertical + horizontal mirror)
    // {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, // 64 ?
    // {0x36, 0x6C, 0x00, 0x6C, 0x76, 0x66, 0x66, 0x00}, // 65 ñ
    // {0x36, 0x6C, 0x00, 0x66, 0x76, 0x7E, 0x6E, 0x66}, // 66 Ñ

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
        case 'C': map_buffer[count++] = 5;  break;  // Antes faltaba
        case 'D': map_buffer[count++] = 27; break;
        case 'E': map_buffer[count++] = 15; break;  // Antes faltaba
        case 'F': map_buffer[count++] = 19; break;
        case 'G': map_buffer[count++] = 42; break;
        case 'I': map_buffer[count++] = 1;  break;  // Antes faltaba
        case 'K': map_buffer[count++] = 29; break;
        case 'M': map_buffer[count++] = 41; break;
        case 'N': map_buffer[count++] = 39; break;  // Antes faltaba
        case 'O': map_buffer[count++] = 30; break;
        case 'P': map_buffer[count++] = 31; break;
        case 'Q': map_buffer[count++] = 61; break;  // CORREGIDO: era 33 (colisión con 'v')
        case 'R': map_buffer[count++] = 32; break;
        case 'S': map_buffer[count++] = 43; break;
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
        /* ── Símbolos ── */
        case ' ': map_buffer[count++] = 4;  break;
        case '!': map_buffer[count++] = 13; break;
        case '.': map_buffer[count++] = 26; break;
        case ':': map_buffer[count++] = 45; break;  // Antes faltaba
        case '?': map_buffer[count++] = 62; break;
        default:  break; // Ignora caracteres no soportados
        }
    }
    return count;
}

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
    const bsp_display_config_t disp_cfg = {.max_transfer_sz = BSP_LCD_H_RES * 100 * sizeof(uint16_t)};
    bsp_display_new(&disp_cfg, &g_panel_handle, &g_io_handle);
    bsp_display_brightness_init();
    esp_lcd_panel_disp_on_off(g_panel_handle, true);
    bsp_display_brightness_set(50); // Brillo inicial al 50%
    return ESP_OK;
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
    /* Buffer estático: 20 × 320 × 2 = 12 800 bytes en DRAM */
    static uint16_t clear_buf[CLEAR_CHUNK_LINES * BSP_LCD_H_RES];
    memset(clear_buf, 0x00, sizeof(clear_buf));

    int lines_sent = 0;
    while (lines_sent < BSP_LCD_V_RES)
    {
        int chunk = ((lines_sent + CLEAR_CHUNK_LINES) <= BSP_LCD_V_RES)
                        ? CLEAR_CHUNK_LINES
                        : (BSP_LCD_V_RES - lines_sent);
        esp_lcd_panel_draw_bitmap(g_panel_handle,
                                  0, lines_sent,
                                  BSP_LCD_H_RES, lines_sent + chunk,
                                  clear_buf);
        lines_sent += chunk;
    }
#undef CLEAR_CHUNK_LINES
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
static void draw_char_to_buffer(uint16_t *target_buffer, int buffer_width, int offset_x, int offset_y, int char_index, uint16_t color, int scale)
{
    // Recorrer cada fila del carácter (8 filas)
    for (int row = 0; row < CHAR_HEIGHT; row++)
    {
        uint8_t line = font_8x8[char_index][row];
        // Recorrer cada columna del carácter (8 columnas)
        for (int col = 0; col < CHAR_WIDTH; col++)
        {
            // Determinar si el píxel debe estar encendido o apagado
            uint16_t px_color = (line & (0x80 >> col)) ? color : 0x0000;

            // Aplicar la escala (dibujar píxel como un bloque de scale x scale)
            for (int dy = 0; dy < scale; dy++)
            {
                for (int dx = 0; dx < scale; dx++)
                {
                    int final_px = offset_x + (col * scale) + dx;
                    int final_py = offset_y + (row * scale) + dy;
                    target_buffer[final_py * buffer_width + final_px] = px_color;
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
    else
    {
        char_spacing = CHAR_SPACING_SCALE_2X;
    }

    // Calcular dimensiones totales del texto
    int total_width = num_chars * display_char_width + (num_chars - 1) * char_spacing;
    int total_height = display_char_height;

    // Asignar buffer para todo el texto
    uint16_t *full_buffer = malloc(total_width * total_height * sizeof(uint16_t));
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
        draw_char_to_buffer(full_buffer, total_width, char_offset_x, 0, char_map[i], color, scale);
    }

    // Enviar el buffer completo al display
    esp_lcd_panel_draw_bitmap(g_panel_handle, start_x, start_y, start_x + total_width, start_y + total_height, full_buffer);
    free(full_buffer);
}

/**
 * @brief Displays the startup screen sequence.
 *        Shows the initial welcome screen with border, displays the welcome message,
 *        waits for 2 seconds, then shows the "AI'm Camila" text.
 */
void display_startup_screen(void)
{
    clear_screen();
    draw_screen_border(COLOR_CYAN_BGR565, 2);
    display_welcome_message();
    vTaskDelay(pdMS_TO_TICKS(2000)); // Espera 2 segundos para mostrar el siguiente mensaje
    // display_camila_text();
    display_drsimi_text();
}

/**
 * @brief Displays the "AI'm Camila" text on the LCD screen.
 *        Renders the chatbot's name centered on the screen with blue color.
 */
static void display_camila_text(void)
{
    // Mapeo de caracteres para "AI'm Camila"
    int camila_map[] = {0, 1, 2, 3, 4, 5, 6, 3, 8, 7, 6};
    int num_chars = sizeof(camila_map) / sizeof(camila_map[0]);
    int scale = 2;

    // Calcular posición centrada
    int text_height = CHAR_HEIGHT * scale;
    int text_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_2X;
    int text_x = (BSP_LCD_H_RES - text_width) / 2;
    int text_y = (BSP_LCD_V_RES - text_height) / 2;

    display_text(text_x, text_y, camila_map, num_chars, COLOR_BLUE_BGR565, scale);
}

/**
 * @brief Displays the "Dr. Simi" text on the LCD screen.
 *        Renders the text centered on the screen with blue color.
 *        Uses scale 3 (larger than Camila text) since it's shorter.
 *        Position matches the vertical placement of display_camila_text().
 */
static void display_drsimi_text(void)
{
    // Mapeo de caracteres para "Dr. Simi"
    // D  r  .  (space)  S  i  m  i
    int drsimi_map[] = {27, 17, 26, 4, 38, 8, 3, 8};
    int num_chars = sizeof(drsimi_map) / sizeof(drsimi_map[0]);
    int scale = 3; // Más grande que Camila (que usa escala 2)

    // Calcular posición centrada
    int text_height = CHAR_HEIGHT * scale;
    int text_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_3X;
    int text_x = (BSP_LCD_H_RES - text_width) / 2;
    int text_y = (BSP_LCD_V_RES - text_height) / 2;

    display_text(text_x, text_y, drsimi_map, num_chars, COLOR_WHITE_BGR565, scale);
}

/**
 * @brief Displays the welcome message on the LCD screen.
 *        Shows "Welcome!" text centered on the screen above the border area.
 */
static void display_welcome_message(void)
{
    // Mapeo de caracteres para "Welcome!"
    int welcome_map[] = {18, 14, 7, 20, 11, 3, 14, 13};
    int num_chars = sizeof(welcome_map) / sizeof(welcome_map[0]);
    int scale = 3;

    // Calcular dimensiones y posición
    int text_height = CHAR_HEIGHT * scale;
    int text_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_3X;
    int text_x = (BSP_LCD_H_RES - text_width) / 2;
    int text_y = (BSP_LCD_V_RES - text_height) / 2;

    // Ajustar posición vertical considerando el borde superior
    int top_border_bottom_y = 4 + 2;
    int space_above_text = text_y - top_border_bottom_y;
    int welcome_y = top_border_bottom_y + (space_above_text - text_height) / 2;

    display_text(text_x, welcome_y, welcome_map, num_chars, COLOR_CYAN_BGR565, scale);
}

/**
 * @brief Displays the online status message on the LCD screen.
 *        Shows "is online!" text centered below the main title area.
 *        Clears any previous WiFi credential messages before displaying.
 * @param color Text color in BGR565 format.
 */
void display_online_status(uint16_t color)
{
    // Mapeo de caracteres para "is online!"
    int char_map[] = {8, 9, 4, 11, 12, 7, 8, 12, 14, 13};
    int num_chars = sizeof(char_map) / sizeof(char_map[0]);
    int scale = 2;

    // Calcular posición centrada
    int online_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_2X;
    int online_x = (BSP_LCD_H_RES - online_width) / 2;
    int text_y_center = (BSP_LCD_V_RES - (CHAR_HEIGHT * scale)) / 2;
    int online_y = text_y_center + (CHAR_HEIGHT * scale) + 12; // Justo debajo del texto principal

    // Limpiar el área donde estaban los mensajes de WiFi
    int clear_width = 220;
    int clear_height = 44;
    draw_filled_rect((BSP_LCD_H_RES - clear_width) / 2, online_y - 4, clear_width, clear_height, 0x0000);

    display_text(online_x, online_y, char_map, num_chars, color, scale);
}

/**
 * @brief Displays the WiFi credentials prompt message on the LCD screen.
 *        Shows "Enter WiFi credentials" text in two lines, centered on the screen.
 *        Used to prompt the user to provide WiFi connection details via BLE.
 */
void display_wifi_creds(void)
{
    // Primera línea: "Enter WiFi"
    int char_map_l1[] = {15, 12, 16, 14, 17, 4, 18, 8, 19, 8};
    int num_chars_l1 = sizeof(char_map_l1) / sizeof(char_map_l1[0]);

    // Segunda línea: "Credentials"
    int char_map_l2[] = {5, 17, 14, 21, 14, 12, 16, 8, 6, 7, 9};
    int num_chars_l2 = sizeof(char_map_l2) / sizeof(char_map_l2[0]);

    int scale = 2;
    int line_spacing = 4;
    int char_h = CHAR_HEIGHT * scale;

    // Calcular dimensiones de cada línea
    int width_l1 = num_chars_l1 * (CHAR_WIDTH * scale) + (num_chars_l1 - 1) * CHAR_SPACING_SCALE_2X;
    int width_l2 = num_chars_l2 * (CHAR_WIDTH * scale) + (num_chars_l2 - 1) * CHAR_SPACING_SCALE_2X;

    // Centrar cada línea independientemente
    int x_l1 = (BSP_LCD_H_RES - width_l1) / 2;
    int x_l2 = (BSP_LCD_H_RES - width_l2) / 2;

    // Calcular posición vertical
    int text_y_center = (BSP_LCD_V_RES - char_h) / 2;
    int y_l1 = text_y_center + char_h + 12;
    int y_l2 = y_l1 + char_h + line_spacing;

    // Limpiar área antes de mostrar el mensaje
    int total_height = (char_h * 2) + line_spacing;
    int max_width = (width_l1 > width_l2) ? width_l1 : width_l2;
    draw_filled_rect((BSP_LCD_H_RES - max_width) / 2 - 4, y_l1 - 4, max_width + 8, total_height + 8, 0x0000);

    // Mostrar ambas líneas en amarillo
    display_text(x_l1, y_l1, char_map_l1, num_chars_l1, COLOR_YELLOW_BGR565, scale);
    display_text(x_l2, y_l2, char_map_l2, num_chars_l2, COLOR_YELLOW_BGR565, scale);
}

/**
 * @brief Displays an error message on the LCD screen.
 *        Shows "Error!" text centered on the screen in red color,
 *        replacing any previous WiFi credential messages.
 *        Used to indicate WiFi connection failure.
 */
void display_error_message(void)
{
    // Mapeo de caracteres para "Error!"
    int char_map[] = {15, 17, 17, 11, 17, 13};
    int num_chars = sizeof(char_map) / sizeof(char_map[0]);
    int scale = 2;
    int char_h = CHAR_HEIGHT * scale;

    // Calcular dimensiones del texto
    int width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_2X;
    int x = (BSP_LCD_H_RES - width) / 2;

    // Usar la misma posición vertical que display_wifi_creds
    int text_y_center = (BSP_LCD_V_RES - char_h) / 2;
    int y = text_y_center + char_h + 12;

    // Limpiar el área de mensajes anteriores
    int clear_width = 220;
    int clear_height = (char_h * 2) + 12;
    draw_filled_rect((BSP_LCD_H_RES - clear_width) / 2, y - 4, clear_width, clear_height, 0x0000);

    // Mostrar el mensaje de error en rojo
    display_text(x, y, char_map, num_chars, COLOR_RED_BGR565, scale);
}

/**
 * @brief Displays "Resetting..." message after user action in config mode
 *        Clears ALL content except screen borders
 *        Message is centered both horizontally and vertically
 */
void display_resetting_message(void)
{
    // ========================================================================
    // Limpiar TODA la pantalla EXCEPTO los bordes
    // ========================================================================

    // Bordes ocupan ~2px en cada lado (ajusta según tu draw_screen_border)
    int border_width = 2;

    // Limpiar el área interna completa
    draw_filled_rect(6, 6, BSP_LCD_H_RES - 12, BSP_LCD_V_RES - 12, COLOR_BLACK_BGR565);

    // ========================================================================
    // MENSAJE "Resetting..." - Una sola línea, centrado completo
    // ========================================================================

    // R e s e t t i n g . . .
    // 17 14 9 14 16 16 8 12 25 26 26 26
    int resetting_map[] = {32, 14, 9, 14, 16, 16, 8, 12, 25, 26, 26, 26};
    int num_resetting = sizeof(resetting_map) / sizeof(resetting_map[0]);

    int scale = 2;
    int msg_height = CHAR_HEIGHT * scale; // 32px

    // Calcular ancho del mensaje
    int msg_width = num_resetting * (CHAR_WIDTH * scale) +
                    (num_resetting - 1) * CHAR_SPACING_SCALE_2X;

    // ========================================================================
    // CENTRADO HORIZONTAL
    // ========================================================================
    int x_msg = (BSP_LCD_H_RES - msg_width) / 2;

    // ========================================================================
    // CENTRADO VERTICAL
    // Teniendo en cuenta el espacio útil (sin bordes)
    // ========================================================================
    int usable_height = BSP_LCD_V_RES - (border_width * 2);
    int y_msg = border_width + ((usable_height - msg_height) / 2);

    // Mostrar el mensaje en YELLOW (indica proceso en curso)
    display_text(x_msg, y_msg, resetting_map, num_resetting,
                 COLOR_YELLOW_BGR565, scale);

    ESP_LOGI(TAG, "Resetting message displayed - Screen cleaned, only borders remain");
}

/**
 * @brief Displays a "Disconnected!" message on the LCD screen.
 *        Shows the text in red color, centered below the WiFi credentials prompt area.
 *        Used to indicate that the device is not connected to WiFi.
 */
void display_disconnected_message(void)
{
    // Mapeo de caracteres para "Disconnected!"
    // D-i-s-c-o-n-n-e-c-t-e-d-!
    int char_map[] = {27, 8, 9, 20, 11, 12, 12, 14, 20, 16, 14, 21, 13};
    int num_chars = sizeof(char_map) / sizeof(char_map[0]);
    int scale = 2;
    int char_h = CHAR_HEIGHT * scale;

    // Calcular dimensiones del texto
    int width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_2X;
    int x = (BSP_LCD_H_RES - width) / 2;

    // Usar la misma posición vertical que display_wifi_creds
    int text_y_center = (BSP_LCD_V_RES - char_h) / 2;
    int y = text_y_center + char_h + 8;

    // Limpiar el área de mensajes anteriores
    int clear_width = 220; // Más ancho para "Disconnected!"
    int clear_height = (char_h * 2) + 12;
    draw_filled_rect((BSP_LCD_H_RES - clear_width) / 2, y - 4, clear_width, clear_height, 0x0000);

    // Mostrar el mensaje de desconexión en rojo
    display_text(x, y, char_map, num_chars, COLOR_RED_BGR565, scale);
}

/**
 * @brief Displays the configuration mode screen with instructions.
 *        Shows the main title and helpful instructions for the user.
 *        OPTIMIZADO: Mensajes más grandes (escala 2) y mensajes ajustados
 *
 * Layout (320x240):
 * ┌──────────────────────┐
 * │    Config Mode       │  ← Azul, escala 2
 * │                      │
 * │  Use app to:         │  ← Blanco, escala 2
 * │  Add WiFi Creds      │  ← Blanco, escala 2
 * │  Add API Key         │  ← Blanco, escala 2
 * │  Clear NVS           │  ← Blanco, escala 2
 * │                      │
 * └──────────────────────┘
 */
void display_config_mode_message(void)
{
    // Limpiar toda la pantalla
    clear_screen();

    // ========================================================================
    // BORDE EXTERNO: Linea cian de 2px
    // ========================================================================
    draw_screen_border(COLOR_CYAN_BGR565, 2); // Borde exterior azul

    // ========================================================================
    // SECCIÓN PRINCIPAL: "Config Mode"
    // ========================================================================

    int config_mode_map[] = {5, 11, 12, 28, 8, 25, 4, 41, 11, 21, 14};
    int num_config = sizeof(config_mode_map) / sizeof(config_mode_map[0]);
    int scale_main = 2;

    int config_width = num_config * (CHAR_WIDTH * scale_main) +
                       (num_config - 1) * CHAR_SPACING_SCALE_2X;
    int config_x = (BSP_LCD_H_RES - config_width) / 2;
    int config_y = 20;

    display_text(config_x, config_y, config_mode_map, num_config,
                 COLOR_BLUE_BGR565, scale_main);

    // ========================================================================
    // LÍNEA DIVISORIA MEJORADA: Doble línea degradada
    // ========================================================================

    int divider_y = config_y + (CHAR_HEIGHT * scale_main) + 12;
    int divider_width = 210;
    int divider_x = (BSP_LCD_H_RES - divider_width) / 2;

    // Línea superior (cian más clara)
    draw_filled_rect(divider_x, divider_y, divider_width, 1, COLOR_CYAN_BGR565);
    // Línea inferior (azul más oscuro)
    draw_filled_rect(divider_x, divider_y + 2, divider_width, 1, COLOR_BLUE_BGR565);

    // ========================================================================
    // CÁLCULO DE ESPACIADO VERTICAL CENTRADO
    // ========================================================================

    int scale_info = 2;
    int info_y_start = divider_y + 28;
    int line_height = (CHAR_HEIGHT * scale_info) + 14;

    int total_messages_height = line_height * 4;
    int space_after_messages = BSP_LCD_V_RES - (info_y_start + total_messages_height);

    if (space_after_messages > info_y_start - divider_y - 28)
    {
        info_y_start = divider_y + ((BSP_LCD_V_RES - divider_y - total_messages_height) / 2);
    }

    // ========================================================================
    // Línea 0: "Use app to:"
    // ========================================================================
    int line0_map[] = {44, 9, 14, 4, 0, 34, 34, 4, 16, 11, 45};
    int num_line0 = sizeof(line0_map) / sizeof(line0_map[0]);

    int line0_width = num_line0 * (CHAR_WIDTH * scale_info) +
                      (num_line0 - 1) * CHAR_SPACING_SCALE_2X;
    int line0_x = (BSP_LCD_H_RES - line0_width) / 2;

    display_text(line0_x, info_y_start, line0_map, num_line0,
                 COLOR_YELLOW_BGR565, scale_info);

    // ========================================================================
    // Línea 1: "Add WiFi Creds" CON FONDO SUTIL
    // ========================================================================
    int line1_map[] = {0, 21, 21, 4, 18, 8, 19, 8, 4, 5, 17, 14, 21, 9};
    int num_line1 = sizeof(line1_map) / sizeof(line1_map[0]);

    int line1_width = num_line1 * (CHAR_WIDTH * scale_info) +
                      (num_line1 - 1) * CHAR_SPACING_SCALE_2X;
    int line1_x = (BSP_LCD_H_RES - line1_width) / 2;

    // Fondo sutil detrás del texto (muy discreto)
    // draw_filled_rect(line1_x - 8, info_y_start + line_height - 2,
    //                  line1_width + 16, 36, COLOR_DARK_BLUE_BGR565);

    display_text(line1_x, info_y_start + line_height, line1_map, num_line1,
                 COLOR_WHITE_BGR565, scale_info);

    // ========================================================================
    // Línea 2: "Add API Key" CON FONDO SUTIL
    // ========================================================================
    int line2_map[] = {0, 21, 21, 4, 0, 31, 1, 4, 29, 14, 24};
    int num_line2 = sizeof(line2_map) / sizeof(line2_map[0]);

    int line2_width = num_line2 * (CHAR_WIDTH * scale_info) +
                      (num_line2 - 1) * CHAR_SPACING_SCALE_2X;
    int line2_x = (BSP_LCD_H_RES - line2_width) / 2;

    // Fondo sutil detrás del texto
    // draw_filled_rect(line2_x - 8, info_y_start + (line_height * 2) - 2,
    //                  line2_width + 16, 36, COLOR_DARK_BLUE_BGR565);

    display_text(line2_x, info_y_start + (line_height * 2), line2_map, num_line2,
                 COLOR_WHITE_BGR565, scale_info);

    // ========================================================================
    // Línea 3: "Clear NVS" CON FONDO SUTIL
    // ========================================================================
    int line3_map[] = {5, 7, 14, 6, 17, 4, 39, 46, 38};
    int num_line3 = sizeof(line3_map) / sizeof(line3_map[0]);

    int line3_width = num_line3 * (CHAR_WIDTH * scale_info) +
                      (num_line3 - 1) * CHAR_SPACING_SCALE_2X;
    int line3_x = (BSP_LCD_H_RES - line3_width) / 2;

    display_text(line3_x, info_y_start + (line_height * 3), line3_map, num_line3,
                 COLOR_WHITE_BGR565, scale_info);
}

/**
 * @brief Displays API Key error message with clean, centered layout
 *        All text in red, blue divider line exactly at screen midpoint
 *
 * Layout (320x240 screen):
 * ┌─────────────────────────┐
 * │   [Blue border]         │
 * │                         │
 * │       Missing or        │  ← Red, scale 2
 * │    Invalid API Key      │  ← Red, scale 2
 * │                         │
 * │    ───────────────      │  ← Blue line at Y=120 (exact center)
 * │                         │
 * │     Send new key        │  ← Yellow, scale 2
 * │        via app          │  ← Yellow, scale 2
 * │                         │
 * └─────────────────────────┘
 */
void display_api_key_error_message(void)
{
    // ========================================================================
    // PASO 1: Limpiar área central preservando borde azul
    // ========================================================================
    const int border_margin = 6;
    draw_filled_rect(border_margin, border_margin,
                     BSP_LCD_H_RES - (border_margin * 2),
                     BSP_LCD_V_RES - (border_margin * 2),
                     0x0000);

    // ========================================================================
    // CONFIGURACIÓN
    // ========================================================================
    const int TEXT_SCALE = 2;
    const int char_h = CHAR_HEIGHT * TEXT_SCALE;
    const int line_spacing = 6;

    // Calcular posición de la línea divisoria
    const int divider_y = BSP_LCD_V_RES / 2;
    const int divider_width = 200;
    const int divider_thickness = 2;
    const int divider_x = (BSP_LCD_H_RES - divider_width) / 2;

    // ========================================================================
    // SECCIÓN SUPERIOR: Mensaje de error reorganizado
    // ========================================================================

    // Línea 1: "Missing or" - 10 caracteres
    int line1_map[] = {41, 8, 9, 9, 8, 12, 25, 4, 11, 17};
    int num_line1 = sizeof(line1_map) / sizeof(line1_map[0]);

    // Línea 2: "Invalid API Key" - 15 caracteres
    // I n v a l i d (space) A P I (space) K e y
    int line2_map[] = {1, 12, 33, 6, 7, 8, 21, 4, 0, 31, 1, 4, 29, 14, 24};
    int num_line2 = sizeof(line2_map) / sizeof(line2_map[0]);

    // Calcular anchos de cada línea
    int line1_width = num_line1 * (CHAR_WIDTH * TEXT_SCALE) +
                      (num_line1 - 1) * CHAR_SPACING_SCALE_2X;
    int line2_width = num_line2 * (CHAR_WIDTH * TEXT_SCALE) +
                      (num_line2 - 1) * CHAR_SPACING_SCALE_2X;

    // Centrar cada línea horizontalmente
    int line1_x = (BSP_LCD_H_RES - line1_width) / 2;
    int line2_x = (BSP_LCD_H_RES - line2_width) / 2;

    // Calcular altura total de las dos líneas
    int error_section_height = (char_h * 2) + line_spacing;

    // Centrar la sección de error en la mitad superior
    int upper_section_height = divider_y - border_margin;
    int error_start_y = border_margin + (upper_section_height - error_section_height) / 2;

    int line1_y = error_start_y;
    int line2_y = line1_y + char_h + line_spacing;

    // Dibujar líneas de error en rojo
    display_text(line1_x, line1_y, line1_map, num_line1, COLOR_RED_BGR565, TEXT_SCALE);
    display_text(line2_x, line2_y, line2_map, num_line2, COLOR_RED_BGR565, TEXT_SCALE);

    // ========================================================================
    // LÍNEA DIVISORIA
    // ========================================================================
    draw_filled_rect(divider_x, divider_y - (divider_thickness / 2),
                     divider_width, divider_thickness,
                     COLOR_CYAN_BGR565);

    // ========================================================================
    // SECCIÓN INFERIOR: Instrucciones (sin cambios)
    // ========================================================================

    // Línea 3: "Send new key"
    int line3_map[] = {38, 14, 12, 21, 4, 12, 14, 22, 4, 37, 14, 24};
    int num_line3 = sizeof(line3_map) / sizeof(line3_map[0]);

    int line3_width = num_line3 * (CHAR_WIDTH * TEXT_SCALE) +
                      (num_line3 - 1) * CHAR_SPACING_SCALE_2X;
    int line3_x = (BSP_LCD_H_RES - line3_width) / 2;

    // Línea 4: "via app"
    int line4_map[] = {33, 8, 6, 4, 6, 34, 34};
    int num_line4 = sizeof(line4_map) / sizeof(line4_map[0]);

    int line4_width = num_line4 * (CHAR_WIDTH * TEXT_SCALE) +
                      (num_line4 - 1) * CHAR_SPACING_SCALE_2X;
    int line4_x = (BSP_LCD_H_RES - line4_width) / 2;

    // Calcular altura total de las instrucciones
    int instruction_section_height = (char_h * 2) + line_spacing;

    // Centrar la sección de instrucciones en la mitad inferior
    int lower_section_height = (BSP_LCD_V_RES - border_margin) - divider_y;
    int instruction_start_y = divider_y + (lower_section_height - instruction_section_height) / 2;

    int line3_y = instruction_start_y;
    int line4_y = line3_y + char_h + line_spacing;

    // Dibujar líneas de instrucciones en amarillo
    display_text(line3_x, line3_y, line3_map, num_line3, COLOR_YELLOW_BGR565, TEXT_SCALE);
    display_text(line4_x, line4_y, line4_map, num_line4, COLOR_YELLOW_BGR565, TEXT_SCALE);

    ESP_LOGI(TAG, "API Key error message displayed with reorganized layout");
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
        bsp_display_brightness_set(brightness);
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms entre cada paso
    }

    // SOLO apagar el backlight, NO el panel LCD
    // Esto evita conflictos con recursos compartidos del SPI
    bsp_display_brightness_set(0);

    // NO llamar esp_lcd_panel_disp_on_off() aquí para evitar conflictos
    // ESP_LOGI(TAG, "LCD backlight turned off safely (panel remains active).");
}

/**
 * @brief Turns the LCD backlight back on.
 *        Restores the display brightness to working level.
 */
void ui_backlight_on(void)
{
    bsp_display_brightness_set(50); // Restaurar brillo al 50%
    ESP_LOGI(TAG, "LCD backlight turned on.");
}

/**
 * @brief Draws a filled rectangle on the LCD screen.
 *        Renders a solid rectangle with the specified color and dimensions.
 *        Includes bounds checking to prevent drawing outside screen boundaries.
 * @param x X position of the rectangle's top-left corner.
 * @param y Y position of the rectangle's top-left corner.
 * @param width Width of the rectangle in pixels.
 * @param height Height of the rectangle in pixels.
 * @param color Fill color in BGR565 format.
 */
/**
 * @brief Dibuja un rectángulo sólido en el LCD.
 *        Usa un buffer de fila en el stack (máx 320×2 = 640 bytes):
 *        no tiene estado estático, elimina la race condition y el leak del buffer anterior.
 */
static void draw_filled_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!g_panel_handle || width <= 0 || height <= 0)
        return;

    if (x >= BSP_LCD_H_RES || y >= BSP_LCD_V_RES)
        return;

    if (x + width  > BSP_LCD_H_RES) width  = BSP_LCD_H_RES - x;
    if (y + height > BSP_LCD_V_RES) height = BSP_LCD_V_RES - y;

    /* Buffer de fila en stack: BSP_LCD_H_RES × 2 = 640 bytes máx — seguro para ESP32-S3 */
    uint16_t row_buf[BSP_LCD_H_RES];
    for (int i = 0; i < width; i++)
        row_buf[i] = color;

    for (int row = 0; row < height; row++)
    {
        esp_lcd_panel_draw_bitmap(g_panel_handle,
                                  x,       y + row,
                                  x + width, y + row + 1,
                                  row_buf);
    }
}

/**
 * @brief Draws a border around the LCD screen edges.
 *        Creates a decorative border with specified color and thickness,
 *        leaving a margin from the actual screen edges.
 * @param color Border color in BGR565 format.
 * @param thickness Border thickness in pixels.
 */
static void draw_screen_border(uint16_t color, int thickness)
{
    const int margin = 4; // Margen desde los bordes de la pantalla

    // Calcular coordenadas del borde
    int x0 = margin;
    int y0 = margin;
    int x1 = BSP_LCD_H_RES - margin;
    int y1 = BSP_LCD_V_RES - margin;

    // Dibujar los cuatro lados del borde
    draw_filled_rect(x0, y0, x1 - x0, thickness, color);                   // Borde superior
    draw_filled_rect(x0, y1 - thickness, x1 - x0, thickness, color);       // Borde inferior
    draw_filled_rect(x0 - 3, y0, thickness, y1 - y0, color);               // Borde izquierdo
    draw_filled_rect((x1 - thickness) - 1, y0, thickness, y1 - y0, color); // Borde derecho
}

/**
 * @brief Muestra un mensaje de estado dinámico en la pantalla con color personalizado.
 *        Posiciona el mensaje debajo del área de WebRTC status.
 *        Permite reutilización con diferentes mensajes y colores.
 *
 * @param message Mensaje a mostrar (ej: "Getting info...", "Muted", etc.)
 * @param color Color del texto en formato BGR565
 */
void ui_show_status_message(const char *message, uint16_t color)
{
    ui_clear_status_message();
    if (!message)
        return;

    int char_map[32];
    int num_chars = convert_string_to_char_map(message, char_map, 32);
    if (num_chars == 0)
        return;

    int scale = 2;
    int online_text_y_center = (BSP_LCD_V_RES - (CHAR_HEIGHT * scale)) / 2;
    int online_text_y = online_text_y_center + (CHAR_HEIGHT * scale) + 12;
    int online_text_height = CHAR_HEIGHT * scale;
    int status_y = online_text_y + online_text_height + 20;

    int status_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_2X;
    int status_x = (BSP_LCD_H_RES - status_width) / 2;
    int status_height = CHAR_HEIGHT * scale;

    // ⭐ ÁREA DE LIMPIEZA: TODO EL ANCHO (respetando bordes de la pantalla)
    int border_width = 6;                                            // Los bordes ocupan ~6px (doble línea azul + cian)
    int extended_height = status_height + 8 + (CHAR_HEIGHT * 1) + 4; // Main + separación + ayuda + margen

    g_status_msg_x = border_width; // ⭐ Desde el borde interno
    g_status_msg_y = status_y - 2;
    g_status_msg_w = BSP_LCD_H_RES - (border_width * 2); // ⭐ Todo el ancho menos bordes
    g_status_msg_h = extended_height + 4;

    // ESP_LOGI(TAG, "Mostrando mensaje de estado: '%s' (área limpieza: %dx%d px)", message, g_status_msg_w, g_status_msg_h);

    display_text(status_x, status_y, char_map, num_chars, color, scale);
}

void ui_clear_status_message(void)
{
    if (g_status_msg_w <= 0)
    {
        return;
    }

    // ESP_LOGI(TAG, "Limpiando mensaje de estado (área: %dx%d px).", g_status_msg_w, g_status_msg_h);
    draw_filled_rect(g_status_msg_x, g_status_msg_y, g_status_msg_w, g_status_msg_h, COLOR_BLACK_BGR565);

    g_status_msg_w = 0;
    g_status_msg_h = 0;
}

/**
 * @brief Displays a help message below the status message on the LCD screen.
 *        Centers the message horizontally and positions it below the status area.
 * @param message Help message to display.
 * @param color Text color in BGR565 format.
 */
void ui_show_help_message_below_status(const char *message, uint16_t color)
{
    ui_clear_help_message_below_status();

    if (!message)
        return;

    // --- 1️⃣ Crear copia segura y sanitizar ---
    char sanitized[128];
    strncpy(sanitized, message, sizeof(sanitized) - 1);
    sanitized[sizeof(sanitized) - 1] = '\0';
    ui_sanitize_text(sanitized);

    int char_map[32];
    int num_chars = convert_string_to_char_map(sanitized, char_map, 32);
    if (num_chars == 0)
        return;

    int scale = 1;

    int online_text_y_center = (BSP_LCD_V_RES - (CHAR_HEIGHT * 2)) / 2;
    int online_text_y = online_text_y_center + (CHAR_HEIGHT * 2) + 12;
    int online_text_height = CHAR_HEIGHT * 2;
    int status_y = online_text_y + online_text_height + 20;
    int status_height = CHAR_HEIGHT * 2;

    int help_y = status_y + status_height + 8;

    int help_width = num_chars * (CHAR_WIDTH * scale) + (num_chars - 1) * CHAR_SPACING_SCALE_1X;
    int help_x = (BSP_LCD_H_RES - help_width) / 2;

    // ⭐ GUARDAR ÁREA DE LIMPIEZA (de borde a borde)
    int border_width = 6; // Respetar bordes de pantalla

    g_help_msg_x = border_width;                       // Desde el borde interno
    g_help_msg_y = help_y - 2;                         // Con margen superior
    g_help_msg_w = BSP_LCD_H_RES - (border_width * 2); // Todo el ancho menos bordes
    g_help_msg_h = (CHAR_HEIGHT * scale) + 4;          // Alto del mensaje + margen

    // ESP_LOGI(TAG, "Mostrando mensaje de ayuda: '%s' (ancho: %d px)", message, help_width);
    display_text(help_x, help_y, char_map, num_chars, color, scale);
}

/**
 * @brief Clears the help message area below the status message.
 *        Resets the area to black and clears stored dimensions.
 */
void ui_clear_help_message_below_status(void)
{
    if (g_help_msg_w <= 0 || g_help_msg_h <= 0)
    {
        return;
    }

    // ESP_LOGI(TAG, "Limpiando mensaje de ayuda (área: %dx%d px).", g_help_msg_w, g_help_msg_h);
    draw_filled_rect(g_help_msg_x, g_help_msg_y, g_help_msg_w, g_help_msg_h, COLOR_BLACK_BGR565);

    // Resetear dimensiones
    g_help_msg_w = 0;
    g_help_msg_h = 0;
}