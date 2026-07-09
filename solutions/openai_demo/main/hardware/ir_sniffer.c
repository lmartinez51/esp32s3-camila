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
#include "ui/simi.h"
#include "esp_claw_init.h"

static const char *TAG = "IR_SNIFFER";


// Mode State
static ir_sniffer_mode_t s_ir_mode = IR_MODE_RECEIVER;
static TimerHandle_t s_learning_timer = NULL;
static int s_learning_remaining_seconds = 15;

// Spinlock for protecting state variable transitions across the Tmr Svc task
// (ir_learning_timeout_cb) and the lua_worker task (ir_sniffer_start_learning).
// portMUX_TYPE spinlocks are safe in timer callbacks; they do NOT block.
static portMUX_TYPE s_ir_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Deferred UI flags removed: using direct try-lock instead.

static void ir_learning_timeout_cb(TimerHandle_t xTimer) {
    // ── Read current state under spinlock (no blocking allowed here) ──────────
    taskENTER_CRITICAL(&s_ir_spinlock);
    bool in_learning = (s_ir_mode == IR_MODE_LEARNING);
    taskEXIT_CRITICAL(&s_ir_spinlock);

    if (!in_learning) return;

    // ── Decrement the countdown ───────────────────────────────────────────────
    taskENTER_CRITICAL(&s_ir_spinlock);
    if (s_learning_remaining_seconds > 0) {
        s_learning_remaining_seconds--;
    }
    int remaining = s_learning_remaining_seconds;
    taskEXIT_CRITICAL(&s_ir_spinlock);

    if (remaining > 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "Waiting: %ds", remaining);
        ui_simi_try_set_arbiter_slot(1, buffer); // Safe try-lock, zero block time
        return;
    }

    // ── Timeout reached: transition state under spinlock ─────────────────────
    bool did_timeout = false;
    taskENTER_CRITICAL(&s_ir_spinlock);
    if (s_ir_mode == IR_MODE_LEARNING) {
        s_ir_mode = IR_MODE_RECEIVER;
        did_timeout = true;
    }
    taskEXIT_CRITICAL(&s_ir_spinlock);

    if (did_timeout) {
        ui_simi_try_set_arbiter_slot(1, ""); // Clear immediately
        
        xTimerStop(s_learning_timer, 0); 
        ESP_LOGW(TAG, "Learning mode timed out! Reverting to RECEIVER mode.");

        esp_claw_rule_t* req = calloc(1, sizeof(esp_claw_rule_t));
        if (req) {
            strlcpy(req->trigger, LUA_CMD_IR_TIMEOUT, sizeof(req->trigger));
            if (esp_claw_send_rule(req) != ESP_OK) {
                free(req);
            }
        }
    }
}

void ir_sniffer_start_learning(void) {
    if (!s_learning_timer) return;

    // ── Transition state under spinlock ──────────────────────────────────────
    // This prevents the timeout callback from racing against this re-arm and
    // immediately overwriting the new IR_MODE_LEARNING state back to RECEIVER.
    taskENTER_CRITICAL(&s_ir_spinlock);
    s_ir_mode = IR_MODE_LEARNING;
    s_learning_remaining_seconds = 15;
    taskEXIT_CRITICAL(&s_ir_spinlock);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Waiting: %ds", 15);
    ui_simi_set_arbiter_slot(1, buffer);

    xTimerReset(s_learning_timer, 0); // Reset timer if already running; start if not.
    ESP_LOGI(TAG, "IR Sniffer Mode changed to: IR_MODE_LEARNING (15s timeout)");
}

#define IR_RX_GPIO_NUM 38
#define IR_TX_GPIO_NUM 39
#define IR_PWR_GPIO_NUM 44
#define RMT_RX_CHANNEL_RESOLUTION_HZ 1000000 // 1MHz resolution, 1 tick = 1us
#define RMT_TX_CHANNEL_RESOLUTION_HZ 1000000

static rmt_channel_handle_t rx_chan = NULL;
static QueueHandle_t receive_queue = NULL;

static rmt_channel_handle_t tx_chan = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;
static QueueHandle_t s_ir_tx_queue = NULL;

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
                    
                    if (s_ir_mode == IR_MODE_LEARNING) {
                        bool timer_needs_stop = false;
                        taskENTER_CRITICAL(&s_ir_spinlock);
                        if (s_ir_mode == IR_MODE_LEARNING) {
                            s_ir_mode = IR_MODE_RECEIVER;
                            timer_needs_stop = true;
                        }
                        taskEXIT_CRITICAL(&s_ir_spinlock);

                        if (timer_needs_stop) {
                            xTimerStop(s_learning_timer, pdMS_TO_TICKS(10));
                            ui_simi_set_arbiter_slot(1, ""); // Clear slot on success
                            ESP_LOGI(TAG, "✅ Learned Code: 0x%08lX. Reverting to RECEIVER.", (unsigned long)scan_code);
                            
                            esp_claw_rule_t* req = calloc(1, sizeof(esp_claw_rule_t));
                            if (req) {
                                strlcpy(req->trigger, LUA_CMD_IR_LEARNED, sizeof(req->trigger));
                                req->num_actions = 1;
                                snprintf(req->actions[0].target, sizeof(req->actions[0].target), "0x%08lX", (unsigned long)scan_code);
                                if (esp_claw_send_rule(req) != ESP_OK) {
                                    free(req);
                                }
                            }
                        }
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
                                } else if (action == IR_ACTION_OUTFIT_RED) {
                                    ui_simi_set_outfit(OUTFIT_CHAPULIN_RED);
                                } else if (action == IR_ACTION_OUTFIT_GREEN) {
                                    ui_simi_set_outfit(OUTFIT_SELECCION_GREEN);
                                } else if (action == IR_ACTION_OUTFIT_WHITE) {
                                    ui_simi_set_outfit(OUTFIT_DOCTOR_WHITE);
                                } else if (action == IR_ACTION_OUTFIT_BARCA) {
                                    ui_simi_set_outfit(OUTFIT_FC_BARCELONA);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

#define NEC_HEADER_HIGH_US 9000
#define NEC_HEADER_LOW_US  4500
#define NEC_BIT_MARK_US    562
#define NEC_SPACE_ZERO_US  562
#define NEC_SPACE_ONE_US   1687
#define NEC_STOP_SPACE_US  10000 // 10ms trailing gap

static void simi_ir_tx_task(void *arg) {
    uint32_t hex_code;
    while (1) {
        if (xQueueReceive(s_ir_tx_queue, &hex_code, portMAX_DELAY) == pdTRUE) {
            rmt_symbol_word_t symbols[34];
            memset(symbols, 0, sizeof(symbols));
            
            // Header
            symbols[0].duration0 = NEC_HEADER_HIGH_US;
            symbols[0].level0 = 1;
            symbols[0].duration1 = NEC_HEADER_LOW_US;
            symbols[0].level1 = 0;
            
            // 32 Data bits (LSB first)
            for (int i = 0; i < 32; i++) {
                symbols[i + 1].duration0 = NEC_BIT_MARK_US;
                symbols[i + 1].level0 = 1;
                symbols[i + 1].level1 = 0;
                
                if ((hex_code >> i) & 1) {
                    symbols[i + 1].duration1 = NEC_SPACE_ONE_US;
                } else {
                    symbols[i + 1].duration1 = NEC_SPACE_ZERO_US;
                }
            }
            
            // Stop bit
            symbols[33].duration0 = NEC_BIT_MARK_US;
            symbols[33].level0 = 1;
            symbols[33].duration1 = NEC_STOP_SPACE_US;
            symbols[33].level1 = 0;
            
            rmt_transmit_config_t tx_config = { .loop_count = 0 };
            esp_err_t err = rmt_transmit(tx_chan, copy_encoder, symbols, sizeof(symbols), &tx_config);
            if (err == ESP_OK) {
                rmt_tx_wait_all_done(tx_chan, -1);
            } else {
                ESP_LOGE(TAG, "RMT Transmit failed: %s", esp_err_to_name(err));
            }
        }
    }
}

esp_err_t ir_transmitter_send_raw(uint32_t hex_code) {
    if (!s_ir_tx_queue) return ESP_ERR_INVALID_STATE;
    
    if (xQueueSend(s_ir_tx_queue, &hex_code, 0) == pdTRUE) {
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "IR TX Queue full! Command dropped.");
        return ESP_ERR_TIMEOUT;
    }
}



esp_err_t ir_sniffer_init(void) {


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


    // Initialize the 1-second learning timeout timer
    s_learning_timer = xTimerCreate("ir_learn_tmr", pdMS_TO_TICKS(1000), pdTRUE, NULL, ir_learning_timeout_cb);

    // Create the diagnostic background sniffer task with increased stack to prevent overflows.
    if (xTaskCreate(ir_rx_task, "ir_rx_task", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }

    s_ir_tx_queue = xQueueCreate(5, sizeof(uint32_t));
    if (s_ir_tx_queue) {
        if (xTaskCreate(simi_ir_tx_task, "ir_tx_task", 3072, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR TX Task");
        }
    }

    return ESP_OK;
}
