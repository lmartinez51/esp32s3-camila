#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for the AHT20 device
 */
typedef struct aht20_dev_t* aht20_dev_handle_t;

/**
 * @brief Initialize the AHT20 sensor on an existing I2C master bus
 * 
 * @param bus_handle The existing ESP-IDF v5 I2C master bus handle
 * @param aht20_handle Pointer to store the initialized AHT20 device handle
 * @return ESP_OK on success, or an error code
 */
esp_err_t aht20_init(i2c_master_bus_handle_t bus_handle, aht20_dev_handle_t *aht20_handle);

/**
 * @brief Read temperature and humidity from AHT20.
 * 
 * This function triggers a measurement and uses a non-blocking delay 
 * (vTaskDelay) of 80ms to yield the CPU while waiting for the sensor 
 * to complete its measurement. This prevents I2C bus freezing/clock stretching.
 * 
 * @param aht20_handle The initialized AHT20 device handle
 * @param temperature Pointer to float to store temperature in Celsius (can be NULL)
 * @param humidity Pointer to float to store relative humidity in % (can be NULL)
 * @return ESP_OK on success, or an error code
 */
esp_err_t aht20_read_temp_humid(aht20_dev_handle_t aht20_handle, float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif
