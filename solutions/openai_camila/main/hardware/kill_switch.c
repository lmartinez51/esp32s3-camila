#include "kill_switch.h"
#include "esp_log.h"
#include "esp_system.h"
#include "iot_button.h"
#include "bsp/esp-box-3.h"
#include "driver/i2c_master.h"
#include "nvs_setup.h"
#include "orchestrator_helpers.h"

static const char *TAG = "KillSwitch";

#define GT911_I2C_ADDRESS 0x5D
#define GT911_REG_STATUS_H 0x81
#define GT911_REG_STATUS_L 0x4E
#define GT911_REG_KEY_H 0x80
#define GT911_REG_KEY_L 0x93

static i2c_master_dev_handle_t s_gt911_dev = NULL;
static uint8_t s_last_key_state = 0;

static button_driver_t s_gt911_btn_driver;

static uint8_t custom_gt911_get_key_level(button_driver_t *button_driver)
{
    if (!s_gt911_dev) return 0;

    uint8_t reg[2] = {GT911_REG_STATUS_H, GT911_REG_STATUS_L};
    uint8_t status = 0;

    if (i2c_master_transmit_receive(s_gt911_dev, reg, 2, &status, 1, 100) == ESP_OK) {
        if ((status & 0x10) == 0x10) { 
            /* Key event flag */
            uint8_t key_reg[2] = {GT911_REG_KEY_H, GT911_REG_KEY_L};
            uint8_t key_val = 0;
            if (i2c_master_transmit_receive(s_gt911_dev, key_reg, 2, &key_val, 1, 100) == ESP_OK) {
                s_last_key_state = (key_val > 0) ? 1 : 0;
            }
            /* Clear the status flag to allow new interrupts/updates */
            uint8_t clear_buf[3] = {GT911_REG_STATUS_H, GT911_REG_STATUS_L, 0x00};
            i2c_master_transmit(s_gt911_dev, clear_buf, 3, 100);
        } else if ((status & 0x80) == 0x80) { 
            /* Touch data ready (or release event) */
            s_last_key_state = 0;
            uint8_t clear_buf[3] = {GT911_REG_STATUS_H, GT911_REG_STATUS_L, 0x00};
            i2c_master_transmit(s_gt911_dev, clear_buf, 3, 100);
        }
    }
    
    return s_last_key_state;
}

static esp_err_t custom_gt911_button_del(button_driver_t *button_driver)
{
    return ESP_OK;
}

static void kill_switch_long_press_cb(void *arg, void *usr_data)
{
    ESP_LOGE(TAG, "KILL SWITCH ACTIVATED: Hardware Long Press Detected!");
    ESP_LOGE(TAG, "Writing CENTINELA mode to NVS and rebooting...");
    
    nvs_set_operation_mode(BOOT_MODE_CENTINELA);
    
    /* Ensure logging completes before restart */
    vTaskDelay(pdMS_TO_TICKS(100)); 
    esp_restart();
}

esp_err_t kill_switch_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "Failed to get I2C bus handle from BSP");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_I2C_ADDRESS,
        .scl_speed_hz = 400000, 
    };

    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_gt911_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GT911 to I2C bus");
        return ESP_FAIL;
    }

    s_gt911_btn_driver.get_key_level = custom_gt911_get_key_level;
    s_gt911_btn_driver.del = custom_gt911_button_del;

    const button_config_t btn_cfg = {
        .long_press_time = 2000,
        .short_press_time = 50,
    };

    button_handle_t btn = NULL;
    esp_err_t err = iot_button_create(&btn_cfg, &s_gt911_btn_driver, &btn);
    if (err != ESP_OK || !btn) {
        ESP_LOGE(TAG, "Failed to create iot_button for kill switch");
        return ESP_FAIL;
    }

    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, kill_switch_long_press_cb, NULL);
    
    ESP_LOGI(TAG, "Global Kill Switch initialized successfully (2s Long Press on Red Circle).");
    return ESP_OK;
}
