#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aht30_dev_t* aht30_dev_handle_t;

esp_err_t aht30_init(i2c_master_bus_handle_t bus_handle, aht30_dev_handle_t *aht30_handle);
esp_err_t aht30_read_temp_humid(aht30_dev_handle_t aht30_handle, float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif
