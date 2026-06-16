#include "aht20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "AHT20";

#define AHT20_I2C_ADDRESS 0x38

struct aht20_dev_t {
    i2c_master_dev_handle_t i2c_dev;
};

esp_err_t aht20_init(i2c_master_bus_handle_t bus_handle, aht20_dev_handle_t *aht20_handle)
{
    if (!bus_handle || !aht20_handle) return ESP_ERR_INVALID_ARG;

    struct aht20_dev_t *dev = calloc(1, sizeof(struct aht20_dev_t));
    if (!dev) return ESP_ERR_NO_MEM;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device to I2C bus");
        free(dev);
        return err;
    }

    // Wait 40ms after power on
    vTaskDelay(pdMS_TO_TICKS(40));

    // Check status
    uint8_t status = 0;
    
    err = i2c_master_receive(dev->i2c_dev, &status, 1, 100);
    if (err == ESP_OK) {
        if ((status & 0x08) == 0) {
            // Need initialization
            ESP_LOGI(TAG, "Initializing AHT20 calibration...");
            uint8_t init_cmd[] = {0xBE, 0x08, 0x00};
            err = i2c_master_transmit(dev->i2c_dev, init_cmd, sizeof(init_cmd), 100);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send init command");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        ESP_LOGW(TAG, "Failed to probe AHT20 status, check wiring or power");
    }

    *aht20_handle = dev;
    ESP_LOGI(TAG, "AHT20 Initialized successfully on existing I2C bus");
    return ESP_OK;
}

esp_err_t aht20_read_temp_humid(aht20_dev_handle_t dev, float *temperature, float *humidity)
{
    if (!dev || !dev->i2c_dev) return ESP_ERR_INVALID_ARG;

    // Trigger measurement
    uint8_t trigger_cmd[] = {0xAC, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(dev->i2c_dev, trigger_cmd, sizeof(trigger_cmd), 100);
    if (err != ESP_OK) return err;

    // Non-blocking yield for 80ms while sensor measures to prevent clock stretching
    vTaskDelay(pdMS_TO_TICKS(80));

    // Read 6 bytes of data
    uint8_t data[6] = {0};
    err = i2c_master_receive(dev->i2c_dev, data, sizeof(data), 100);
    if (err != ESP_OK) return err;

    // Check busy bit (Bit 7 of byte 0)
    if ((data[0] & 0x80) != 0) {
        ESP_LOGE(TAG, "AHT20 busy after 80ms delay");
        return ESP_ERR_TIMEOUT;
    }

    uint32_t raw_humid = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temp = (((uint32_t)(data[3] & 0x0F)) << 16) | ((uint32_t)data[4] << 8) | data[5];

    if (humidity) {
        *humidity = ((float)raw_humid * 100.0f) / 1048576.0f;
    }
    if (temperature) {
        *temperature = ((float)raw_temp * 200.0f) / 1048576.0f - 50.0f;
    }

    return ESP_OK;
}
