#include "ir_sniffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <string.h>
#include <inttypes.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/timers.h"
#include "ir_action_map.h"
#include "app_events.h"

static const char *TAG = "IR_SNIFFER";

// Pairing State Machine
static bool s_is_pairing = false;
static ir_action_t s_pairing_target = IR_ACTION_NONE;
static TimerHandle_t s_pairing_timer = NULL;

#define IR_RX_GPIO_NUM 38
#define IR_TX_GPIO_NUM 39
#define IR_PWR_GPIO_NUM 44
#define RMT_RX_CHANNEL_RESOLUTION_HZ 1000000 // 1MHz resolution, 1 tick = 1us
#define RMT_TX_CHANNEL_RESOLUTION_HZ 1000000

static rmt_channel_handle_t rx_chan = NULL;
static QueueHandle_t receive_queue = NULL;

static rmt_channel_handle_t tx_chan = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void ir_rx_task(void *arg) {
    ESP_LOGI(TAG, "IR Diagnostic Sniffer task started on GPIO %d", IR_RX_GPIO_NUM);

    // Wait 10ms for RMT line to idle before configuring receive
    vTaskDelay(pdMS_TO_TICKS(10));

    // Safe configuration that respects the 3187ns maximum hardware limit
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,      // 1.25us glitch filter
        .signal_range_max_ns = 30000000,  // 30,000,000 ns (30ms) RX idle timeout threshold
    };

    #define SYMBOLS_MAX 256
    static rmt_symbol_word_t raw_symbols[SYMBOLS_MAX];
    
    while (1) {
        // Start non-blocking receive
        esp_err_t err = rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &receive_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RMT receive failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Wait for RMT to finish receiving a sequence
        rmt_rx_done_event_data_t rx_data;
        if (xQueueReceive(receive_queue, &rx_data, portMAX_DELAY) == pdTRUE) {
            size_t num_symbols = rx_data.num_symbols;
            
            if (num_symbols > 0) {
                if (num_symbols == 2 && raw_symbols[0].duration0 > 8000 && raw_symbols[0].duration1 > 2000) {
                    ESP_LOGI(TAG, "🔁 Repeat Code Detected");
                } else if (num_symbols >= 34 && raw_symbols[0].duration0 > 4000 && raw_symbols[0].duration1 > 4000) {
                    uint32_t scan_code = 0;
                    // Standard NEC and Samsung transmit LSB first.
                    for (int i = 1; i <= 32; i++) {
                        if (raw_symbols[i].duration1 > 1000) {
                            scan_code |= (1UL << (i - 1));
                        }
                    }
                    ESP_LOGW(TAG, "✅ Decoded Hex: 0x%08lX", (unsigned long)scan_code);
                    
                    if (s_is_pairing) {
                        xTimerStop(s_pairing_timer, 0);
                        esp_err_t pair_err = ir_map_add_code(s_pairing_target, scan_code);
                        if (pair_err == ESP_OK) {
                            ESP_LOGW(TAG, "✅ Pairing Success! Code 0x%08lX mapped to action %d.", (unsigned long)scan_code, s_pairing_target);
                            // TODO: Dispatch ORCH_EVENT_IR_PAIRING_SUCCESS to Orchestrator
                        } else {
                            ESP_LOGE(TAG, "❌ Pairing Failed! NVS/Cache Error: %s", esp_err_to_name(pair_err));
                        }
                        s_is_pairing = false;
                        s_pairing_target = IR_ACTION_NONE;
                    } else {
                        ir_action_t action = ir_map_lookup(scan_code);
                        if (action != IR_ACTION_NONE) {
                            static uint64_t last_dispatch_time = 0;
                            uint64_t now = esp_timer_get_time() / 1000;
                            if (now - last_dispatch_time > 500) {
                                ESP_LOGI(TAG, "📡 IR Action Dispatched: %d", action);
                                last_dispatch_time = now;
                                
                                if (action == IR_ACTION_SLEEP || action == IR_ACTION_MUTE) {
                                    orchestrator_post_event(ORCH_EVENT_MIC_MUTED);
                                } else if (action == IR_ACTION_WAKE || action == IR_ACTION_UNMUTE) {
                                    orchestrator_post_event(ORCH_EVENT_MIC_UNMUTED);
                                } else if (action == IR_ACTION_TOGGLE_SLEEP || action == IR_ACTION_TOGGLE_MUTE) {
                                    bool is_muted = orchestrator_get_mute_state();
                                    orchestrator_post_event(is_muted ? ORCH_EVENT_MIC_UNMUTED : ORCH_EVENT_MIC_MUTED);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void ir_transmitter_send_raw(uint32_t hex_code) {
    if (!tx_chan || !copy_encoder) return;
    
    // Future integration can assemble proper 68-symbol NEC frames from hex_code.
    // For now, emit a generic 1ms burst to verify TX functionality.
    rmt_symbol_word_t burst[1];
    memset(burst, 0, sizeof(burst));
    burst[0].duration0 = 1000; burst[0].level0 = 1; burst[0].duration1 = 1000; burst[0].level1 = 0;
    
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    rmt_transmit(tx_chan, copy_encoder, burst, sizeof(burst), &tx_config);
}

static void ir_pairing_timeout_cb(TimerHandle_t xTimer) {
    if (s_is_pairing) {
        s_is_pairing = false;
        s_pairing_target = IR_ACTION_NONE;
        ESP_LOGW(TAG, "Pairing mode timed out! No IR code received in 30 seconds.");
        // TODO: Dispatch ORCH_EVENT_IR_PAIRING_TIMEOUT to Orchestrator
    }
}

void ir_sniffer_enter_pairing_mode(ir_action_t target_action) {
    if (!s_pairing_timer) return;
    
    s_pairing_target = target_action;
    s_is_pairing = true;
    xTimerReset(s_pairing_timer, 0); // Starts or resets the 30-second timer
    
    ESP_LOGI(TAG, "Intercept Mode ACTIVE. Waiting up to 30s for IR signal to map action %d...", target_action);
}

esp_err_t ir_sniffer_init(void) {
    // Initialize the IR Action Map RAM cache and NVS persistence
    esp_err_t map_err = ir_action_map_init();
    if (map_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize IR Action Map: %s", esp_err_to_name(map_err));
        return map_err;
    }

    // Power on the IR Receiver module (P-Channel MOSFET on GPIO 44, Active LOW)
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << IR_PWR_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level((gpio_num_t)IR_PWR_GPIO_NUM, 0);
    
    // Give the sensor a moment to power up and stabilize
    vTaskDelay(pdMS_TO_TICKS(10));

    receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!receive_queue) return ESP_ERR_NO_MEM;

    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RX_CHANNEL_RESOLUTION_HZ,
        .mem_block_symbols = 64, // Sufficient for basic logging
        .gpio_num = IR_RX_GPIO_NUM,
        .flags.invert_in = true, // IR sensor is active-low, invert it to active-high for RMT
    };
    
    esp_err_t err = rmt_new_rx_channel(&rx_chan_config, &rx_chan);
    if (err != ESP_OK) return err;

    // Remove explicit pull-up in case the IR module has a push-pull output
    // gpio_set_pull_mode((gpio_num_t)IR_RX_GPIO_NUM, GPIO_PULLUP_ONLY);

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(rx_chan, &cbs, receive_queue);
    if (err != ESP_OK) return err;

    err = rmt_enable(rx_chan);
    if (err != ESP_OK) return err;

    // Init TX
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_TX_CHANNEL_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .gpio_num = IR_TX_GPIO_NUM,
        .trans_queue_depth = 4,
    };
    err = rmt_new_tx_channel(&tx_chan_config, &tx_chan);
    if (err == ESP_OK) {
        rmt_carrier_config_t carrier_cfg = {
            .duty_cycle = 0.33, // Standard 33% duty cycle for IR transmission
            .frequency_hz = 38000, // 38kHz NEC
            .flags.always_on = false,
        };
        rmt_apply_carrier(tx_chan, &carrier_cfg);
        
        rmt_copy_encoder_config_t copy_encoder_config = {};
        rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder);
        rmt_enable(tx_chan);
    }

    // Initialize the 30-second intercept timeout timer (one-shot)
    s_pairing_timer = xTimerCreate("ir_pair_tmr", pdMS_TO_TICKS(30000), pdFALSE, NULL, ir_pairing_timeout_cb);

    // Create the diagnostic background sniffer task with increased stack to prevent overflows.
    if (xTaskCreate(ir_rx_task, "ir_rx_task", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
