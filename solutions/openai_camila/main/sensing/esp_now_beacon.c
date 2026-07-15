#include "esp_now_beacon.h"
#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "ESP_NOW_BEACON";

/* Max payload size for ESP-NOW is 250 bytes */
#define ESP_NOW_MAX_PAYLOAD_LEN 250

/* Queue config */
#define ESP_NOW_QUEUE_SIZE 10

/* Raw payload struct for the FreeRTOS Queue */
typedef struct {
    uint8_t mac_addr[6];
    uint8_t data[ESP_NOW_MAX_PAYLOAD_LEN];
    int data_len;
} esp_now_queue_evt_t;

/* FreeRTOS Queue Handle */
static QueueHandle_t s_esp_now_queue = NULL;

/**
 * @brief ISR-safe ESP-NOW receive callback.
 *        Executes in the Wi-Fi task context. Strictly no blocking/JSON parsing.
 */
static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (s_esp_now_queue == NULL || recv_info == NULL || data == NULL || len <= 0) {
        return;
    }

    if (len > ESP_NOW_MAX_PAYLOAD_LEN) {
        len = ESP_NOW_MAX_PAYLOAD_LEN; // Truncate safely
    }

    esp_now_queue_evt_t evt;
    memset(&evt, 0, sizeof(esp_now_queue_evt_t));
    
    // Copy sender MAC and payload
    memcpy(evt.mac_addr, recv_info->src_addr, 6);
    memcpy(evt.data, data, len);
    evt.data_len = len;

    // Send to queue, yielding if a higher priority task was woken
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_esp_now_queue, &evt, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief FreeRTOS task handling CSI beaconing and ESP-NOW command parsing.
 */
static void csi_beacon_task(void *pvParameter)
{
    esp_now_queue_evt_t evt;
    bool is_beacon_active = false;

    /* Dummy 802.11 QoS Data frame header + payload */
    uint8_t raw_payload[] = {
        0x88, 0x00, // Frame Control (QoS Data)
        0x00, 0x00, // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Address 1 (Destination)
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, // Address 2 (Source)
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, // Address 3 (BSSID)
        0x00, 0x00, // Sequence Control
        0x00, 0x00, // QoS Control
        'C', 'S', 'I', '_', 'C', 'A', 'M', 'I', 'L', 'A' // Payload
    };

    while (1) {
        /* If active, don't block on the queue (0 timeout) to maintain 50ms pacing loop below. 
           If inactive, block for 50ms to yield CPU. */
        TickType_t wait_ticks = is_beacon_active ? 0 : pdMS_TO_TICKS(50);

        if (xQueueReceive(s_esp_now_queue, &evt, wait_ticks) == pdTRUE) {
            // Null-terminate the data payload securely for cJSON
            char json_str[ESP_NOW_MAX_PAYLOAD_LEN + 1];
            memcpy(json_str, evt.data, evt.data_len);
            json_str[evt.data_len] = '\0';

            cJSON *root = cJSON_Parse(json_str);
            if (root) {
                cJSON *token = cJSON_GetObjectItem(root, "token");
                if (cJSON_IsString(token) && strcmp(token->valuestring, "camila_secure") == 0) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
                    if (cJSON_IsString(cmd)) {
                        if (strcmp(cmd->valuestring, "start_beacon") == 0) {
                            is_beacon_active = true;
                            ESP_LOGI(TAG, "CSI Beacon Started by %02x:%02x:%02x:%02x:%02x:%02x", 
                                     evt.mac_addr[0], evt.mac_addr[1], evt.mac_addr[2], 
                                     evt.mac_addr[3], evt.mac_addr[4], evt.mac_addr[5]);
                        } else if (strcmp(cmd->valuestring, "stop_beacon") == 0) {
                            is_beacon_active = false;
                            ESP_LOGI(TAG, "CSI Beacon Stopped by %02x:%02x:%02x:%02x:%02x:%02x", 
                                     evt.mac_addr[0], evt.mac_addr[1], evt.mac_addr[2], 
                                     evt.mac_addr[3], evt.mac_addr[4], evt.mac_addr[5]);
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "ESP-NOW packet rejected: Invalid or missing token.");
                }
                cJSON_Delete(root); // PREVENT MEMORY LEAK
            }
        }

        if (is_beacon_active) {
            // Inject raw 802.11 packet (WIFI_IF_STA is the primary interface)
            esp_wifi_80211_tx(WIFI_IF_STA, raw_payload, sizeof(raw_payload), true);
            
            // Strict pacing to yield to WebRTC and maintain 20Hz beacon rate
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/**
 * @brief Initializes the ESP-NOW receiver and CSI Beacon task.
 */
esp_err_t esp_now_beacon_init(void)
{
    static bool is_initialized = false;
    if (is_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESP-NOW CSI Beacon...");

    // 1. Create Queue FIRST to prevent ISR race conditions
    s_esp_now_queue = xQueueCreate(ESP_NOW_QUEUE_SIZE, sizeof(esp_now_queue_evt_t));
    if (s_esp_now_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW queue");
        return ESP_FAIL;
    }

    // 2. Disable Wi-Fi Power Save to guarantee ESP-NOW packet reception
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // 3. Initialize ESP-NOW
    size_t dma_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(esp_now_init());
    size_t dma_after = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "[HEAP] ESP-NOW init cost: %zu bytes (DMA free before: %zu, after: %zu)", 
             (dma_before > dma_after) ? (dma_before - dma_after) : 0, 
             dma_before, dma_after);
    
    // 4. Register Receive Callback
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));

    // 5. Spawn the CSI Beacon Task on Core 1 (APP_CPU) to leave Core 0 for Wi-Fi
    // Priority 4 (assuming WebRTC audio is higher priority)
    xTaskCreatePinnedToCore(csi_beacon_task, "csi_beacon", 4096, NULL, 4, NULL, 1);

    is_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW CSI Beacon Initialized Successfully.");
    
    return ESP_OK;
}
