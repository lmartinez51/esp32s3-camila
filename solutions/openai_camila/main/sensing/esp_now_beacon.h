#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the ESP-NOW receiver and CSI Beacon task.
 * 
 * Must be called after Wi-Fi is initialized and connected to the AP.
 * Configures Wi-Fi power save mode to WIFI_PS_NONE for reliable ESP-NOW rx.
 */
esp_err_t esp_now_beacon_init(void);

#ifdef __cplusplus
}
#endif
