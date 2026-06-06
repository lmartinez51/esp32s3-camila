#ifndef MAIN_UI_H
#define MAIN_UI_H
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Inicializa el LCD, configura backlight y limpia pantalla.
     * @return ESP_OK o código de error.
     */
    esp_err_t ui_init(void);

    /**
     * @brief Muestra la pantalla de inicio con mensaje de bienvenida.
     * Dibuja borde azul, mensaje de bienvenida y espera 2 segundos.
     */
    void display_startup_screen(void);

    /**
     * @brief Dibuja "is online!" debajo del mensaje principal.
     * @param color Color en BGR565.
     */
    void display_online_status(uint16_t color);

    /**
     * @brief Muestra un mensaje para que el usuario ingrese credenciales WiFi vía BLE.
     * Aparece en la misma posición que el estado "online".
     */
    void display_wifi_creds(void);

    /**
     * @brief Muestra un mensaje de error en la pantalla LCD.
     * Reemplaza el mensaje de credenciales WiFi con "Error!".
     */
    void display_error_message(void);

    /**
     * @brief Muestra un mensaje indicando que el dispositivo se está reiniciando.
     * Limpia el área debajo de la línea divisoria y muestra "Resetting...".
     */
    void display_resetting_message(void);

    /**
     * @brief Muestra un mensaje de desconexión.
     * Indica que el dispositivo está desconectado de la red WiFi.
     */
    void display_disconnected_message(void);

    /**
     * @brief Muestra un mensaje de error de clave API en la pantalla LCD.
     * Indica que la clave API es inválida o falta.
     */
    void display_api_key_error_message(void);

    /**
     * @brief Apaga el backlight del LCD y desactiva el panel.
     */
    void ui_backlight_off_safe(void);

    /**
     * @brief Muestra un mensaje indicando que el dispositivo está en modo configuración.
     * Limpia la pantalla y muestra "MODO CONFIG" centrado.
     */
    void display_config_mode_message(void);

    /**
     * @brief Enciende el backlight del LCD.
     * Restaura el brillo al 50% para uso normal.
     */
    void ui_backlight_on(void);

    /**
     * @brief Muestra un mensaje de estado temporal en la pantalla.
     * Aparece debajo del mensaje "is online!".
     * @param message El texto a mostrar (e.g., "getting prices...").
     * @param color Color del texto en formato BGR565
     */
    void ui_show_status_message(const char *message, uint16_t color);

    /**
     * @brief Borra el mensaje de estado actual de la pantalla.
     * Limpia el área donde se mostró el último mensaje de estado.
     */
    void ui_clear_status_message(void);

    /**
     * @brief Muestra un mensaje de ayuda debajo del mensaje de estado actual.
     *
     * IMPORTANTE: Este mensaje se limpiará automáticamente cuando se llame
     * a ui_clear_status_message(), ya que el área de limpieza está ampliada.
     *
     * @param message Texto a mostrar (máximo 22 caracteres recomendado)
     * @param color Color del texto en formato BGR565
     */
    void ui_show_help_message_below_status(const char *message, uint16_t color);

    /**
     * @brief Limpia el área del mensaje de ayuda (de borde a borde).
     */
    void ui_clear_help_message_below_status(void);

    /**
     * @brief Sanitizes a text string by removing or replacing unsupported UTF-8 characters.
     *        Specifically removes inverted exclamation and question marks, and replaces
     *        accented characters with their unaccented equivalents.
     * @param text The input text string to sanitize (modified in place).
     */
    void ui_sanitize_text(char *text);

// Colores útiles en formato BGR565
#define COLOR_GREEN_BGR565 0x001F
#define COLOR_RED_BGR565 0x07E0
#define COLOR_BLUE_BGR565 0x0F800
#define COLOR_WHITE_BGR565 0xFFFF
#define COLOR_BLACK_BGR565 0x0000
#define COLOR_YELLOW_BGR565 0x07FF
#define COLOR_CYAN_BGR565 0xFFE0
#define COLOR_MAGENTA_BGR565 0xF81F
#define COLOR_DARK_BLUE_BGR565 0x0400

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_H */
