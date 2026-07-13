#ifndef MAIN_UI_H
#define MAIN_UI_H
#include <stdint.h>
#include <stdbool.h>
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
     * @brief Libera temporalmente el panel LCD y el bus SPI asociado.
     */
    esp_err_t ui_deinit(void);

    /**
     * @brief Libera el panel LCD/SPI sin apagar backlight ni enviar disp_off.
     *        Intenta dejar visible el ultimo frame mientras otros subsistemas usan RAM/SPI.
     */
    esp_err_t ui_deinit_keep_last_frame(void);

    /**
     * @brief Indica si el panel LCD está inicializado y listo para dibujar.
     */
    bool ui_is_initialized(void);

    /**
     * @brief Muestra la pantalla de inicio con mensaje de bienvenida.
     * Dibuja borde azul, mensaje de bienvenida y espera 2 segundos.
     */
    void display_startup_screen(void);

    /**
     * @brief Muestra bienvenida personalizada tras validar identidad BLE.
     */
    void display_welcome_identity(const char *name);

    /**
     * @brief Muestra una pantalla ligera de fase del sistema.
     */
    void display_system_phase_message(const char *title, const char *subtitle, uint16_t color);

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
     * @brief Muestra alerta roja de intruso detectado.
     * Limpia la pantalla, dibuja un borde rojo grueso y texto centrado a escala máxima.
     */
    void display_intruder_alert_message(void);

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
     * @brief Enciende el backlight del LCD para asegurar que la pantalla sea visible.
     */
    void ui_backlight_on(void);

    /**
     * @brief Muestra un mensaje de estado temporal en la pantalla.
     * Aparece debajo del retrato de Dr. Simi.
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

    void ui_draw_text_to_buffer(uint16_t *buffer, int buffer_w, int buffer_h,
                                int start_x, int start_y,
                                const char *text, uint16_t color, int scale);

    int ui_get_text_width(const char *text, int scale);

    /**
     * @brief Toma el mutex global del panel LCD (bloqueante).
     *        Úsalo para agrupar varios blits de forma atómica frente a otras tareas.
     */
    void ui_panel_lock(void);

    /**
     * @brief Libera el mutex global del panel LCD.
     */
    void ui_panel_unlock(void);

    /**
     * @brief Envía un bitmap al panel protegido por el mutex del LCD.
     * @param x0,y0 Esquina superior izquierda (inclusiva).
     * @param x1,y1 Esquina inferior derecha (exclusiva).
     * @param pixels Buffer de píxeles en el formato del panel (16 bpp).
     */
    void ui_panel_blit(int x0, int y0, int x1, int y1, const void *pixels);

    /**
     * @brief Intenta enviar un bitmap al panel sin esperar indefinidamente por el mutex.
     * @return true si el frame se envio completo; false si el panel estaba ocupado o fallo el flush.
     */
    bool ui_panel_try_blit(int x0, int y0, int x1, int y1, const void *pixels, uint32_t lock_timeout_ms);

    /**
     * @brief Limpia toda la pantalla a negro.
     */
    void ui_clear_screen(void);

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
