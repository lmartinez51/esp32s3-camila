/**
 * @file sensor_dock.c
 * @brief Sensor Dock detection and AHT30 temperature sensor management.
 *
 * Implements external I2C bus initialization, hardware radar dock detection
 * via non-destructive I2C ping, and AHT30 temperature/humidity reads.
 * Follows the "hardware optionality" pattern: all failures are non-fatal.
 */

#include "sensor_dock.h"

#include <stddef.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "aht30.h"
#include "hardware/radar.h"
#include "simi.h"

static const char *TAG = "MAIN";

/* ── I2C Bus Configuration ──────────────────────────────────────────────── */

#define SENSOR_I2C_SDA_PIN 41
#define SENSOR_I2C_SCL_PIN 40

/* ── Module State ───────────────────────────────────────────────────────── */

bool     g_hardware_radar_present = false;
bool     s_aht30_present          = false;
uint32_t s_last_aht30_poll_ms     = 0;

static aht30_dev_handle_t s_aht30_handle = NULL;

/* ── Private Functions ──────────────────────────────────────────────────── */

/**
 * @brief Initialize the external I2C master bus on the sensor dock pins.
 * @return Bus handle on success, NULL on failure.
 */
static i2c_master_bus_handle_t sensor_board_i2c_init(void)
{
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = -1, /* Auto select port */
        .scl_io_num             = SENSOR_I2C_SCL_PIN,
        .sda_io_num             = SENSOR_I2C_SDA_PIN,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize external I2C bus: %s", esp_err_to_name(err));
        return NULL;
    }
    return bus_handle;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool hardware_is_sensor_dock_connected(void)
{
    return g_hardware_radar_present;
}

void sensor_dock_init(void)
{
    ESP_LOGI(TAG, "Initializing External I2C Bus for Sensor Dock (10kHz)...");
    i2c_master_bus_handle_t bus_handle = sensor_board_i2c_init();
    if (!bus_handle) {
        ESP_LOGW(TAG, "Failed to get External I2C handle. Sensor disabled.");
        return;
    }

    /* --- NON-DESTRUCTIVE I2C PING TO SENSOR DOCK (0x28) --- */
    esp_err_t ping_err = i2c_master_probe(bus_handle, 0x28, -1);
    if (ping_err == ESP_OK) {
        g_hardware_radar_present = true;
        ESP_LOGI(TAG, "Sensor Dock detected at 0x28. Hardware Radar Presence ENABLED.");
    } else {
        ESP_LOGI(TAG, "No device at 0x28. Software CSI Presence ENABLED.");
    }
    /* ------------------------------------------------------ */

    if (aht30_init(bus_handle, &s_aht30_handle) == ESP_OK) {
        s_aht30_present = true;
        ESP_LOGI(TAG, "AHT30 initialized successfully. Tolerant Architecture enabled.");
    } else {
        ESP_LOGW(TAG, "AHT30 not found on bus. Hardware optionality triggered (Disabled).");
    }
}

void sensor_dock_poll_temperature(void)
{
    if (!s_aht30_present || !s_aht30_handle) {
        return;
    }

    float     temp = 0.0f;
    esp_err_t err  = aht30_read_temp_humid(s_aht30_handle, &temp, NULL);

    char temp_str[16];
    if (err == ESP_OK) {
        snprintf(temp_str, sizeof(temp_str), "%.1f C", temp);
    } else {
        snprintf(temp_str, sizeof(temp_str), "-- C");
        ESP_LOGW(TAG, "AHT30 read failed mid-session. Continuing gracefully.");
    }

    /* Pass the text to the Dr. Simi UI renderer for safe double-buffered compositing */
    ui_simi_set_temperature_text(temp_str);
}
