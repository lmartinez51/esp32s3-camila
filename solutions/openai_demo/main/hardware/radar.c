#include "hardware/radar.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_events.h"

#define RADAR_GPIO_NUM GPIO_NUM_21
#define RADAR_DEBOUNCE_COOLDOWN_US 500000 // 500ms cooldown

// Extern reference to the central orchestrator event queue
extern QueueHandle_t s_orchestrator_event_queue;

// Tracks the last time the ISR triggered to prevent event flooding
static volatile int64_t s_last_isr_time_us = 0;

/**
 * @brief Hardware ISR handler for the radar presence detection.
 * 
 * Executes strictly in IRAM to prevent cache misses.
 * Operates in ONE-SHOT mode: immediately disables its own interrupt to prevent interrupt storms.
 * Posts an ORCH_EVENT_MOTION_DETECTED event to the Orchestrator safely.
 */
static void IRAM_ATTR radar_isr_handler(void* arg)
{
    // ONE-SHOT: Immediately disable the hardware interrupt to prevent interrupt storms!
    gpio_intr_disable(RADAR_GPIO_NUM);

    int64_t current_time = esp_timer_get_time();
    
    // Check software debounce cooldown
    if (current_time - s_last_isr_time_us < RADAR_DEBOUNCE_COOLDOWN_US) {
        return; // Ignore rapid bouncing or spurious triggers
    }
    
    s_last_isr_time_us = current_time;

    // Send event to the orchestrator queue safely
    if (s_orchestrator_event_queue != NULL) {
        orchestrator_event_msg_t event_msg = {
            .type = ORCH_EVENT_MOTION_DETECTED,
            .timestamp_ms = (uint32_t)(current_time / 1000),
            .corr_drop = false 
        };

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(s_orchestrator_event_queue, &event_msg, &xHigherPriorityTaskWoken);

        // Force an immediate context switch if the Orchestrator task was woken up
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t radar_hal_init(void)
{
    // Configure the GPIO for the radar
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RADAR_GPIO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Enabled per spec
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE    // Interrupt on rising edge
    };
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    // Install global GPIO ISR service (Flag 0 is safe if already installed by other drivers)
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // Attach the specific hardware interrupt handler to our radar pin
    err = gpio_isr_handler_add(RADAR_GPIO_NUM, radar_isr_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    // Keep the interrupt explicitly disabled at boot. The Orchestrator manages the lifecycle.
    gpio_intr_disable(RADAR_GPIO_NUM);

    return ESP_OK;
}

void radar_hal_enable(void)
{
    // Re-setting the interrupt type clears the pending interrupt status internally on ESP-IDF
    // This prevents immediate false-positives upon re-arming
    gpio_set_intr_type(RADAR_GPIO_NUM, GPIO_INTR_POSEDGE);
    gpio_intr_enable(RADAR_GPIO_NUM);
}

void radar_hal_disable(void)
{
    gpio_intr_disable(RADAR_GPIO_NUM);
}
