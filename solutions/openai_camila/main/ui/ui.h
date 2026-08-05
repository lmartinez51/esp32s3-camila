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

// Safe NOP macros & helpers for legacy calls
#define ui_init() (ESP_OK)
#define ui_deinit() (ESP_OK)
#define ui_deinit_keep_last_frame() (ESP_OK)
#define ui_is_initialized() (true)
#define display_startup_screen() do {} while(0)
#define display_welcome_identity(name) do {} while(0)
#define display_system_phase_message(title, subtitle, color) do {} while(0)
#define display_wifi_creds() do {} while(0)
#define display_error_message() do {} while(0)
#define display_resetting_message() do {} while(0)
#define display_disconnected_message() do {} while(0)
#define display_network_timeout_message() do {} while(0)
#define display_api_key_error_message() do {} while(0)
#define display_intruder_alert_message() do {} while(0)
#define display_config_mode_message() do {} while(0)
#define ui_show_status_message(msg, color) do {} while(0)
#define ui_clear_status_message() do {} while(0)
#define ui_show_help_message_below_status(msg, color) do {} while(0)
#define ui_clear_help_message_below_status() do {} while(0)
#define ui_backlight_off_safe() bsp_display_brightness_set(0)
#define ui_backlight_on() bsp_display_brightness_set(100)
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
