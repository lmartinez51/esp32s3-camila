#ifndef KILL_SWITCH_H
#define KILL_SWITCH_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the global Kill Switch using the GT911 touch button.
 *        This registers a custom iot_button that directly polls the GT911 
 *        I2C registers without full touch driver initialization.
 * 
 * @return esp_err_t ESP_OK on success, or an error code upon failure.
 */
esp_err_t kill_switch_init(void);

#ifdef __cplusplus
}
#endif

#endif // KILL_SWITCH_H
