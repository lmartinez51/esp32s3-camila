#pragma once

/**
 * @brief Global UI Configuration
 * 
 * Set to 1 to enable the new LVGL-based parallel rendering engine.
 * Set to 0 to fallback to the legacy manual drawing engine (simi.c).
 */
#define USE_LVGL_UI 0

typedef enum {
    UI_STATE_BOOT = 0,
    UI_STATE_WIFI_CONNECTING,
    UI_STATE_BLE_SCAN,
    UI_STATE_BLE_DISCOVERY,
    UI_STATE_SUCCESS,
    UI_STATE_ACTIVE_WEBRTC,
    UI_STATE_ALERT_VIGILANTE,
    UI_STATE_ERROR
} ui_state_t;
