#ifndef MAIN_UI_H
#define MAIN_UI_H

#include "ui_config.h"
#include "camila_lvgl_ui.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Zero-LVGL UI Prototypes and declarations
esp_err_t ui_init(void);
esp_err_t ui_deinit(void);
esp_err_t ui_deinit_keep_last_frame(void);
bool ui_is_initialized(void);
void display_startup_screen(void);
void display_welcome_identity(const char *name);
void display_system_phase_message(const char *title, const char *subtitle, uint16_t color);
void display_wifi_creds(void);
void display_error_message(void);
void display_resetting_message(void);
void display_disconnected_message(void);
void display_network_timeout_message(void);
void display_api_key_error_message(void);
void display_intruder_alert_message(void);
void display_config_mode_message(void);
void ui_show_status_message(const char *msg, uint16_t color);
void ui_clear_status_message(void);
void ui_show_help_message_below_status(const char *msg, uint16_t color);
void ui_clear_help_message_below_status(void);
void camila_ui_update_state(ui_state_t state, const char* title, const char* subtitle);
void camila_ui_show_avatar(void);
void camila_ui_set_speaking_state(bool is_speaking);
void camila_ui_update_mute_countdown(int remaining_seconds);
void ui_backlight_off_safe(void);
void ui_backlight_on(void);
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
