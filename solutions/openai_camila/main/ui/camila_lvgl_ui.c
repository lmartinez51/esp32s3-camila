#include "ui_config.h"

#ifdef USE_LVGL_UI

#include <string.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "camila_lvgl_ui.h"

static const char *TAG = "LVGL_UI";

SemaphoreHandle_t g_lvgl_mutex = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

// UI Objects
static lv_obj_t *ui_indicator_box = NULL;
static lv_obj_t *ui_title_label = NULL;
static lv_obj_t *ui_subtitle_label = NULL;
static lv_obj_t *ui_help_label = NULL;
static lv_obj_t *ui_accent_line = NULL;

// Equalizer UI Objects (Statically Allocated at Startup, Hidden by Default)
static lv_obj_t *ui_eq_container = NULL;
static lv_obj_t *ui_eq_bars[7] = {NULL};

static lv_timer_t *eq_anim_timer = NULL;
static bool s_is_speaking = false;
static bool s_is_muted = false;
static bool s_is_consulting = false;
static ui_state_t s_current_ui_state = UI_STATE_BOOT;
static uint32_t s_anim_step = 0;

/**
 * @brief DMA flush ready callback. Executed in ISR context when the SPI transfer completes.
 */
IRAM_ATTR bool camila_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

/**
 * @brief LVGL flush callback. Pushes rendered buffer to LCD via DMA.
 */
void camila_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD draw_bitmap failed (%s), backing off...", esp_err_to_name(err));
        lv_disp_flush_ready(drv);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Lightweight 120ms equalizer animation timer callback.
 */
static void eq_anim_timer_cb(lv_timer_t *timer)
{
    if (ui_eq_bars[0] == NULL) return;

    s_anim_step++;

    if (s_is_speaking && s_current_ui_state == UI_STATE_ACTIVE_WEBRTC) {
        // Active speaking modulation (Tall/Thin 4px bars, max height 50px, Violet Neon #9D4EDD & Electric Magenta #FF007F)
        static const uint8_t speaking_heights[6][7] = {
            { 14, 32, 50, 24, 44, 18, 30 },
            { 28, 46, 20, 40, 30, 50, 22 },
            { 44, 18, 36, 14, 48, 26, 38 },
            { 22, 50, 28, 46, 20, 36, 16 },
            { 34, 20, 44, 26, 40, 22, 48 },
            { 18, 38, 24, 48, 22, 42, 26 }
        };
        uint8_t idx = s_anim_step % 6;
        for (int i = 0; i < 7; i++) {
            lv_color_t bar_color = (i % 2 == 0) ? lv_color_hex(0x9D4EDD) : lv_color_hex(0xFF007F);
            lv_obj_set_height(ui_eq_bars[i], speaking_heights[idx][i]);
            lv_obj_set_style_bg_color(ui_eq_bars[i], bar_color, 0);
        }
    } else if (s_is_consulting) {
        // Consulting / Search Mode (Soft Yellow sweep #FFD700 across 7 bars)
        lv_color_t consult_color = lv_color_hex(0xFFD700);
        uint8_t active_bar = s_anim_step % 7;
        for (int i = 0; i < 7; i++) {
            uint8_t h = (i == active_bar) ? 36 : 10;
            lv_obj_set_height(ui_eq_bars[i], h);
            lv_obj_set_style_bg_color(ui_eq_bars[i], consult_color, 0);
        }
    } else {
        // Normal Active WebRTC calm state (Alternating Violet/Magenta, height 8px to 16px)
        static const uint8_t calm_heights[4][7] = {
            { 8, 12, 16, 10, 16, 12, 8 },
            { 10, 16, 10, 16, 10, 16, 10 },
            { 14, 8, 16, 8, 16, 8, 14 },
            { 8, 16, 8, 16, 8, 16, 8 }
        };
        uint8_t idx = s_anim_step % 4;
        for (int i = 0; i < 7; i++) {
            lv_color_t bar_color = (i % 2 == 0) ? lv_color_hex(0x9D4EDD) : lv_color_hex(0xFF007F);
            lv_obj_set_height(ui_eq_bars[i], calm_heights[idx][i]);
            lv_obj_set_style_bg_color(ui_eq_bars[i], bar_color, 0);
        }
    }
}

/**
 * @brief LVGL Main RTOS Task.
 */
static void camila_lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL UI Task...");

    // 1. Create LVGL Mutex
    g_lvgl_mutex = xSemaphoreCreateMutex();
    if (!g_lvgl_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        vTaskDelete(NULL);
        return;
    }

    // 2. Allocate Internal DMA SRAM draw buffer (320 * 10 lines = 6.4 KB)
    lv_color_t *draw_buf_ptr = heap_caps_malloc(320 * 10 * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!draw_buf_ptr) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer in DMA SRAM");
        vTaskDelete(NULL);
        return;
    }

    // 3. Init LVGL Core & Display Driver
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf_ptr, NULL, 320 * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BSP_LCD_H_RES;
    disp_drv.ver_res = BSP_LCD_V_RES;
    disp_drv.flush_cb = camila_lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel_handle;

    lv_disp_drv_register(&disp_drv);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = camila_lvgl_flush_ready_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, &disp_drv);

    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    bsp_display_brightness_set(100);

    // =========================================================
    // Static UI Object Initialization (Created once at startup)
    // =========================================================
    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        // 1. Base Canvas (Black)
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

        // 2. Static 7-Bar Equalizer Container (Centered at Y = -60)
        ui_eq_container = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ui_eq_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(ui_eq_container, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(ui_eq_container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ui_eq_container, 0, 0);
        lv_obj_set_style_pad_all(ui_eq_container, 0, 0);
        lv_obj_set_style_pad_column(ui_eq_container, 4, 0);
        lv_obj_set_layout(ui_eq_container, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(ui_eq_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ui_eq_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(ui_eq_container, LV_ALIGN_CENTER, 0, -60);
        lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_SCROLLABLE);

        // Create 7 static vertical bars (width 4px, radius 2px, gap 4px)
        for (int i = 0; i < 7; i++) {
            ui_eq_bars[i] = lv_obj_create(ui_eq_container);
            lv_obj_set_size(ui_eq_bars[i], 4, 8); // Default 8px height
            lv_color_t initial_color = (i % 2 == 0) ? lv_color_hex(0x9D4EDD) : lv_color_hex(0xFF007F);
            lv_obj_set_style_bg_color(ui_eq_bars[i], initial_color, 0);
            lv_obj_set_style_bg_opa(ui_eq_bars[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ui_eq_bars[i], 0, 0);
            lv_obj_set_style_radius(ui_eq_bars[i], 2, 0);
            lv_obj_clear_flag(ui_eq_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        // STRICT RULE: Force Equalizer HIDDEN during startup/setup states
        lv_obj_add_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);

        // 3. Subtle 1px Horizontal Accent Line (Centered at Y = +90, width 220px)
        ui_accent_line = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ui_accent_line, 220, 1);
        lv_obj_align(ui_accent_line, LV_ALIGN_CENTER, 0, 90);
        lv_obj_set_style_bg_color(ui_accent_line, lv_color_hex(0xFF007F), 0);
        lv_obj_set_style_bg_opa(ui_accent_line, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui_accent_line, 0, 0);
        lv_obj_set_style_radius(ui_accent_line, 0, 0);
        lv_obj_clear_flag(ui_accent_line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);

        // 3. Static Indicator Box (Centered at Y = 15)
        ui_indicator_box = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ui_indicator_box, 260, 100);
        lv_obj_align(ui_indicator_box, LV_ALIGN_CENTER, 0, 15);
        lv_obj_set_style_bg_color(ui_indicator_box, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(ui_indicator_box, 6, 0);
        lv_obj_set_style_border_color(ui_indicator_box, lv_color_hex(0x8A2BE2), 0);
        lv_obj_set_style_radius(ui_indicator_box, 10, 0);
        lv_obj_clear_flag(ui_indicator_box, LV_OBJ_FLAG_SCROLLABLE);

        // 4. Typography: Title
        ui_title_label = lv_label_create(ui_indicator_box);
        lv_obj_set_style_text_font(ui_title_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ui_title_label, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(ui_title_label, "SYSTEM BOOT");
        lv_obj_align(ui_title_label, LV_ALIGN_CENTER, 0, -18);

        // 5. Typography: Subtitle
        ui_subtitle_label = lv_label_create(ui_indicator_box);
        lv_obj_set_style_text_font(ui_subtitle_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_subtitle_label, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(ui_subtitle_label, "Initializing...");
        lv_obj_align(ui_subtitle_label, LV_ALIGN_CENTER, 0, 4);

        // 6. Typography: Help Instruction Label (Shown during Muted mode)
        ui_help_label = lv_label_create(ui_indicator_box);
        lv_obj_set_style_text_font(ui_help_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_help_label, lv_color_hex(0x777777), 0);
        lv_label_set_text(ui_help_label, "Double-press button to unmute");
        lv_obj_align(ui_help_label, LV_ALIGN_CENTER, 0, 26);
        lv_obj_add_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);

        // 6. Create Static Equalizer Timer (120ms interval, PAUSED by default during setup)
        eq_anim_timer = lv_timer_create(eq_anim_timer_cb, 120, NULL);
        lv_timer_pause(eq_anim_timer);

        xSemaphoreGive(g_lvgl_mutex);
    }
    ESP_LOGI(TAG, "LVGL Static UI Initialized (Equalizer OFF during startup).");

    // Handler Loop
    while (1) {
        lv_tick_inc(10);
        if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(g_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Public hook to spawn LVGL parallel task.
 */
void camila_lvgl_init(void)
{
    ESP_LOGI(TAG, "Synchronous Hardware Init (bsp_display_new)");

    const bsp_display_config_t disp_cfg = {.max_transfer_sz = 320 * 10 * sizeof(uint16_t)};
    esp_err_t err = bsp_display_new(&disp_cfg, &s_panel_handle, &s_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LCD synchronously: %s", esp_err_to_name(err));
        return;
    }

    xTaskCreatePinnedToCore(camila_lvgl_task, "lvgl_task", 6144, NULL, 5, NULL, 1);
}

void camila_ui_update_state(ui_state_t state, const char* title, const char* subtitle)
{
    if (g_lvgl_mutex == NULL || ui_indicator_box == NULL) {
        return;
    }

    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        s_current_ui_state = state;

        if (title != NULL) {
            lv_label_set_text(ui_title_label, title);
        }
        if (subtitle != NULL) {
            lv_label_set_text(ui_subtitle_label, subtitle);
        }

        // Check special modes from title / subtitle strings
        bool is_muted_text = (title && strstr(title, "MUTED")) || 
                             (subtitle && strstr(subtitle, "Muted")) || 
                             (subtitle && strstr(subtitle, "Microphone"));
        bool is_consulting_text = (title && strstr(title, "CONSULTING")) || 
                                  (subtitle && strstr(subtitle, "Consulting")) || 
                                  (subtitle && strstr(subtitle, "Getting info")) || 
                                  (subtitle && strstr(subtitle, "Searching"));

        s_is_muted = is_muted_text;
        s_is_consulting = is_consulting_text;

        if (is_muted_text) {
            // Modern MUTE MODE: Frameless, 7 flat 4px red bars, Red Accent Line (#DC143C)
            lv_obj_set_style_border_width(ui_indicator_box, 0, 0);

            if (ui_eq_container) lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_pause(eq_anim_timer);
            for (int i = 0; i < 7; i++) {
                if (ui_eq_bars[i]) {
                    lv_obj_set_height(ui_eq_bars[i], 4);
                    lv_obj_set_style_bg_color(ui_eq_bars[i], lv_color_hex(0xDC143C), 0);
                }
            }

            if (ui_help_label) {
                lv_obj_clear_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(ui_help_label, lv_color_hex(0x777777), 0);
            }

            if (ui_accent_line) {
                lv_obj_clear_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(ui_accent_line, lv_color_hex(0xDC143C), 0);
            }

        } else if (is_consulting_text) {
            // Modern CONSULTING MODE: Frameless, 7-bar Yellow sweep, Yellow Accent Line (#FFD700)
            lv_obj_set_style_border_width(ui_indicator_box, 0, 0);
            if (ui_help_label) lv_obj_add_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);

            if (ui_eq_container) lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_resume(eq_anim_timer);

            if (ui_accent_line) {
                lv_obj_clear_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(ui_accent_line, lv_color_hex(0xFFD700), 0);
            }

        } else if (state == UI_STATE_ACTIVE_WEBRTC) {
            // Modern ACTIVE WEBRTC: Frameless, 7-bar Violet/Magenta Equalizer, Magenta Accent Line (#FF007F)
            lv_obj_set_style_border_width(ui_indicator_box, 0, 0);
            if (ui_help_label) lv_obj_add_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);

            if (ui_eq_container) lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_resume(eq_anim_timer);

            if (ui_accent_line) {
                lv_obj_clear_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(ui_accent_line, lv_color_hex(0xFF007F), 0);
            }

        } else if (state == UI_STATE_ALERT_VIGILANTE) {
            // Modern VIGILANTE / SECURITY ALERT: Frameless, 7 flat Crimson Red bars, Crimson Accent Line (#FF0055)
            lv_obj_set_style_border_width(ui_indicator_box, 0, 0);
            if (ui_help_label) lv_obj_add_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);

            if (ui_eq_container) lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_pause(eq_anim_timer);
            for (int i = 0; i < 7; i++) {
                if (ui_eq_bars[i]) {
                    lv_obj_set_height(ui_eq_bars[i], 4);
                    lv_obj_set_style_bg_color(ui_eq_bars[i], lv_color_hex(0xFF0055), 0);
                }
            }

            if (ui_accent_line) {
                lv_obj_clear_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(ui_accent_line, lv_color_hex(0xFF0055), 0);
            }

        } else {
            // STATIC SETUP STATES: BOOT, WIFI_CONNECTING, BLE_SCAN, SUCCESS, ERROR
            // Hide Modern UI overlay elements
            if (ui_help_label) lv_obj_add_flag(ui_help_label, LV_OBJ_FLAG_HIDDEN);
            if (ui_accent_line) lv_obj_add_flag(ui_accent_line, LV_OBJ_FLAG_HIDDEN);
            if (ui_eq_container) lv_obj_add_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_pause(eq_anim_timer);

            // Restore Static Box Layout with border width 6
            lv_obj_set_style_border_width(ui_indicator_box, 6, 0);
            lv_color_t target_color;
            switch (state) {
                case UI_STATE_BOOT:            target_color = lv_color_hex(0x8A2BE2); break; // Neon Violet
                case UI_STATE_WIFI_CONNECTING: target_color = lv_color_hex(0xFF8C00); break; // Sunset Orange
                case UI_STATE_BLE_SCAN:        target_color = lv_color_hex(0x00FFFF); break; // Cyan
                case UI_STATE_SUCCESS:         target_color = lv_color_hex(0x50C878); break; // Emerald Green
                case UI_STATE_ERROR:           target_color = lv_color_hex(0xFF0000); break; // Red
                default:                       target_color = lv_color_hex(0x8A2BE2); break;
            }
            lv_obj_set_style_border_color(ui_indicator_box, target_color, 0);
        }

        xSemaphoreGive(g_lvgl_mutex);
    }
}

void camila_ui_show_avatar(void)
{
    camila_ui_update_state(UI_STATE_ACTIVE_WEBRTC, "Camila AI", "Listening for your voice...");
}

void camila_ui_set_speaking_state(bool is_speaking)
{
    if (g_lvgl_mutex == NULL) return;

    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        s_is_speaking = is_speaking;
        if (is_speaking) {
            if (ui_eq_container) lv_obj_clear_flag(ui_eq_container, LV_OBJ_FLAG_HIDDEN);
            if (eq_anim_timer) lv_timer_resume(eq_anim_timer);
        } else if (s_is_muted) {
            if (eq_anim_timer) lv_timer_pause(eq_anim_timer);
            for (int i = 0; i < 7; i++) {
                if (ui_eq_bars[i]) {
                    lv_obj_set_height(ui_eq_bars[i], 4);
                    lv_obj_set_style_bg_color(ui_eq_bars[i], lv_color_hex(0xDC143C), 0);
                }
            }
        }
        xSemaphoreGive(g_lvgl_mutex);
    }
}

void camila_ui_update_mute_countdown(int remaining_seconds)
{
    if (g_lvgl_mutex && ui_subtitle_label && s_is_muted) {
        if (xSemaphoreTake(g_lvgl_mutex, 0) == pdTRUE) {
            char buf[32];
            int mins = remaining_seconds / 60;
            int secs = remaining_seconds % 60;
            snprintf(buf, sizeof(buf), "Auto-reset in: (%02d:%02d)", mins, secs);
            lv_label_set_text(ui_subtitle_label, buf);
            xSemaphoreGive(g_lvgl_mutex);
        }
    }
}

#endif // USE_LVGL_UI
