#include "ui_config.h"

#ifdef USE_LVGL_UI

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

// External image declarations (placed here to keep the header clean for main.c)
LV_IMG_DECLARE(camila_base);
LV_IMG_DECLARE(boca_cerrada);
LV_IMG_DECLARE(boca_abierta);

static lv_obj_t *ui_indicator_box = NULL;
static lv_obj_t *ui_title_label = NULL;
static lv_obj_t *ui_subtitle_label = NULL;

static lv_obj_t *ui_camila_bg = NULL;
static lv_obj_t *ui_camila_mouth = NULL;

static lv_timer_t *mouth_timer = NULL;
static bool mouth_is_open = false;

/**
 * @brief DMA flush ready callback. Executed in ISR context when the SPI transfer completes.
 *        Notifies LVGL that it is safe to reuse the display buffer or render the next chunk.
 * 
 * @param panel_io   LCD panel IO handle
 * @param edata      Event data (unused here)
 * @param user_ctx   User data (we pass the LVGL display driver ptr)
 * @return true if a high priority task has been woken up, false otherwise.
 */
IRAM_ATTR bool camila_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    
    // Signal to LVGL that the DMA transfer is complete
    lv_disp_flush_ready(disp_drv);
    
    // Return false since lv_disp_flush_ready does not natively require an RTOS yield
    return false;
}

/**
 * @brief LVGL flush callback. Pushes the rendered LVGL buffer to the display via DMA.
 *        This function returns immediately; the actual SPI transfer is asynchronous.
 * 
 * @param drv        LVGL display driver configuration
 * @param area       The rectangular area on the screen to update
 * @param color_map  The rendered pixel data buffer
 */
void camila_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // Retrieve the LCD panel handle from the driver's user_data
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    
    // Push the bitmap to the display hardware. 
    // esp_lcd_panel_draw_bitmap expects the end coordinates to be exclusive (+1).
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

/**
 * @brief LVGL Main RTOS Task. Initializes the LCD, LVGL core, and runs the handler loop.
 */
static void camila_lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL UI Task...");

    // 1. Create the LVGL Global Mutex
    g_lvgl_mutex = xSemaphoreCreateMutex();
    if (!g_lvgl_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        vTaskDelete(NULL);
        return;
    }

    // 3. Allocate the LVGL draw buffer in PSRAM (Reduced from 40 to 10 lines)
    lv_color_t *draw_buf_ptr = heap_caps_malloc(320 * 10 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!draw_buf_ptr) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    // 4. Initialize LVGL core
    lv_init();

    // 5. Configure Draw Buffer (Reduced chunk size)
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf_ptr, NULL, 320 * 10);

    // 6. Configure Display Driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BSP_LCD_H_RES;
    disp_drv.ver_res = BSP_LCD_V_RES;
    disp_drv.flush_cb = camila_lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel_handle; // Pass panel handle to flush_cb

    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    // 7. Register the ISR flush_ready callback mapping the disp_drv context
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = camila_lvgl_flush_ready_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, &disp_drv);

    // Turn on backlight and panel
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    bsp_display_brightness_set(100);

    // =========================================================
    // TASK 6: Dynamic State-Driven UI Setup (Lightweight)
    // =========================================================
    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        // 1. Base Canvas (Black)
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

        // 2. Static Indicator Box (Replaces heavy arc)
        ui_indicator_box = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ui_indicator_box, 260, 100);
        lv_obj_center(ui_indicator_box);
        lv_obj_set_style_bg_color(ui_indicator_box, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(ui_indicator_box, 6, 0);
        lv_obj_set_style_border_color(ui_indicator_box, lv_color_hex(0x8A2BE2), 0); // Boot color
        lv_obj_set_style_radius(ui_indicator_box, 10, 0);
        lv_obj_clear_flag(ui_indicator_box, LV_OBJ_FLAG_SCROLLABLE);

        // 3. Typography: Title
        ui_title_label = lv_label_create(ui_indicator_box);
        lv_obj_set_style_text_font(ui_title_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ui_title_label, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(ui_title_label, "SYSTEM BOOT");
        lv_obj_align(ui_title_label, LV_ALIGN_CENTER, 0, -15);

        // 4. Typography: Subtitle
        ui_subtitle_label = lv_label_create(ui_indicator_box);
        lv_obj_set_style_text_font(ui_subtitle_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui_subtitle_label, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(ui_subtitle_label, "Initializing...");
        lv_obj_align(ui_subtitle_label, LV_ALIGN_CENTER, 0, 15);
        
        xSemaphoreGive(g_lvgl_mutex);
    }
    ESP_LOGI(TAG, "LVGL Static UI Initialized.");

    // =========================================================
    // TASK 5: LVGL Tick and Handler Loop
    // =========================================================
    while (1) {
        // Manually increment the tick (since we aren't hooking esp_timer here)
        lv_tick_inc(10);
        
        // Lock mutex to prevent collisions with other RTOS tasks
        if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(g_lvgl_mutex);
        }
        
        // Strict yield to prevent starving WebRTC/WiFi tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Public hook to spawn the LVGL parallel UI engine.
 */
void camila_lvgl_init(void)
{
    ESP_LOGI(TAG, "Synchronous Hardware Init (bsp_display_new)");
    
    // 1. Initialize LCD panel via BSP sequentially to prevent I2C race conditions
    // Reduced max_transfer_sz to 320 * 10 (6.4 KB) to free ~19.2 KB of DMA RAM for the BLE controller.
    const bsp_display_config_t disp_cfg = {.max_transfer_sz = 320 * 10 * sizeof(uint16_t)};
    esp_err_t err = bsp_display_new(&disp_cfg, &s_panel_handle, &s_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LCD synchronously: %s", esp_err_to_name(err));
        return;
    }

    // Spawn LVGL task with a reasonable stack and lower priority than audio/WiFi
    xTaskCreatePinnedToCore(camila_lvgl_task, "lvgl_task", 6144, NULL, 5, NULL, 1);
}

void camila_ui_update_state(ui_state_t state, const char* title, const char* subtitle)
{
    if (g_lvgl_mutex == NULL || ui_indicator_box == NULL) {
        return;
    }
    
    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        
        if (title != NULL) {
            lv_label_set_text(ui_title_label, title);
        }
        if (subtitle != NULL) {
            lv_label_set_text(ui_subtitle_label, subtitle);
        }
        
        lv_color_t target_color;

        switch (state) {
            case UI_STATE_BOOT:          target_color = lv_color_hex(0x8A2BE2); break; // Neon Violet
            case UI_STATE_WIFI_CONNECTING: target_color = lv_color_hex(0xFF8C00); break; // Sunset Orange
            case UI_STATE_BLE_SCAN:      target_color = lv_color_hex(0x00FFFF); break; // Cyan
            case UI_STATE_SUCCESS:       target_color = lv_color_hex(0x50C878); break; // Emerald
            case UI_STATE_ACTIVE_WEBRTC: target_color = lv_color_hex(0xF5F5F5); break; // Soft White
            case UI_STATE_ALERT_VIGILANTE: target_color = lv_color_hex(0xDC143C); break; // Crimson
            case UI_STATE_ERROR:         target_color = lv_color_hex(0xFF0000); break; // Red
            default:                     target_color = lv_color_hex(0x8A2BE2); break;
        }
        
        lv_obj_set_style_border_color(ui_indicator_box, target_color, 0);
        
        xSemaphoreGive(g_lvgl_mutex);
    }
}

static void mouth_anim_timer_cb(lv_timer_t * timer)
{
    if (ui_camila_mouth == NULL) return;
    
    // Toggle the mouth state
    mouth_is_open = !mouth_is_open;
    
    if (mouth_is_open) {
        lv_img_set_src(ui_camila_mouth, &boca_abierta);
    } else {
        lv_img_set_src(ui_camila_mouth, &boca_cerrada);
    }
}

void camila_ui_show_avatar(void)
{
    if (g_lvgl_mutex == NULL) return;

    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        
        // 1. Hide the boot UI safely
        if (ui_indicator_box != NULL) {
            lv_obj_add_flag(ui_indicator_box, LV_OBJ_FLAG_HIDDEN);
        }

        // 2. Instantiate the Static Background
        if (ui_camila_bg == NULL) {
            ui_camila_bg = lv_img_create(lv_scr_act());
            lv_img_set_src(ui_camila_bg, &camila_base);
            lv_obj_align(ui_camila_bg, LV_ALIGN_TOP_LEFT, 0, 0);
        } else {
            lv_obj_clear_flag(ui_camila_bg, LV_OBJ_FLAG_HIDDEN);
        }

        // 3. Instantiate the Mouth Sprite overlay
        if (ui_camila_mouth == NULL) {
            ui_camila_mouth = lv_img_create(lv_scr_act());
            lv_img_set_src(ui_camila_mouth, &boca_cerrada);
            // Placeholder coordinates: X=148, Y=136
            lv_obj_align(ui_camila_mouth, LV_ALIGN_TOP_LEFT, 148, 136); 
            
            // 4. Create the mouth animation timer (paused by default)
            mouth_timer = lv_timer_create(mouth_anim_timer_cb, 350, NULL);
            lv_timer_pause(mouth_timer);
        } else {
            lv_obj_clear_flag(ui_camila_mouth, LV_OBJ_FLAG_HIDDEN);
        }

        xSemaphoreGive(g_lvgl_mutex);
    }
}

void camila_ui_set_speaking_state(bool is_speaking)
{
    if (g_lvgl_mutex == NULL || ui_camila_mouth == NULL || mouth_timer == NULL) return;

    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        
        if (is_speaking) {
            // Resume the rhythmic mouth animation
            lv_timer_resume(mouth_timer);
        } else {
            // Pause the animation and force the mouth closed
            lv_timer_pause(mouth_timer);
            mouth_is_open = false;
            lv_img_set_src(ui_camila_mouth, &boca_cerrada);
        }

        xSemaphoreGive(g_lvgl_mutex);
    }
}

#endif // USE_LVGL_UI
