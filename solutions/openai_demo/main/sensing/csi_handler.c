#include "csi_handler.h"
#include "ei_inference.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#define CSI_RINGBUF_STORAGE_SIZE (4 * 1024)
#define CSI_FEATURE_BYTES 64
#define CSI_CAPTURE_SAMPLE_BYTES CSI_FEATURE_BYTES
#define CSI_CONSUMER_STACK_SIZE 3072
#define CSI_CONSUMER_PRIORITY (tskIDLE_PRIORITY + 7)
#define CSI_CONSUMER_CORE_ID 1
#define WINDOW_SIZE 20
#define DEV_THRESHOLD 45
#define PEAK_COUNT_TRIGGER 5
#define TIME_WINDOW_FRAMES 50
#define COOLDOWN_FRAMES 100

#define CSI_MAC_FILTER_ENABLED 0
#define CSI_ALLOWED_MAC_0 \
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#define CSI_ALLOWED_MAC_1 \
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

typedef struct {
    uint8_t mac[6];
    uint8_t dmac[6];
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint8_t secondary_channel;
    uint16_t csi_len;
    uint16_t sample_len;
    uint16_t payload_len;
    uint16_t rx_seq;
    uint32_t rx_timestamp;
    bool first_word_invalid;
    int8_t csi_sample[CSI_CAPTURE_SAMPLE_BYTES];
} csi_frame_record_t;

static const char *TAG = "CSI";

static DRAM_ATTR uint8_t s_csi_ringbuf_storage[CSI_RINGBUF_STORAGE_SIZE] __attribute__((aligned(4)));
static DRAM_ATTR StaticRingbuffer_t s_csi_ringbuf_struct;
static RingbufHandle_t s_csi_ringbuf;
static TaskHandle_t s_csi_consumer_task;
static volatile uint32_t s_csi_dropped_frames;
static volatile bool s_csi_enabled;
static uint32_t moving_window[WINDOW_SIZE] = {0};
static uint8_t window_index = 0;
static uint32_t window_sum = 0;

static bool csi_mac_allowed(const uint8_t mac[6])
{
#if CSI_MAC_FILTER_ENABLED
    static const uint8_t allowed_mac_0[6] = CSI_ALLOWED_MAC_0;
    static const uint8_t allowed_mac_1[6] = CSI_ALLOWED_MAC_1;

    return memcmp(mac, allowed_mac_0, sizeof(allowed_mac_0)) == 0 ||
           memcmp(mac, allowed_mac_1, sizeof(allowed_mac_1)) == 0;
#else
    (void)mac;
    return true;
#endif
}

static void csi_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;

    if (data == NULL || data->buf == NULL || s_csi_ringbuf == NULL) {
        return;
    }

    if (!csi_mac_allowed(data->mac)) {
        return;
    }

    csi_frame_record_t frame;
    frame.rssi = data->rx_ctrl.rssi;
    frame.noise_floor = data->rx_ctrl.noise_floor;
    frame.channel = data->rx_ctrl.channel;
    frame.secondary_channel = data->rx_ctrl.secondary_channel;
    frame.csi_len = data->len;
    frame.payload_len = data->payload_len;
    frame.rx_seq = data->rx_seq;
    frame.rx_timestamp = data->rx_ctrl.timestamp;
    frame.first_word_invalid = data->first_word_invalid;
    memcpy(frame.mac, data->mac, sizeof(frame.mac));
    memcpy(frame.dmac, data->dmac, sizeof(frame.dmac));

    frame.sample_len = data->len < CSI_CAPTURE_SAMPLE_BYTES ? data->len : CSI_CAPTURE_SAMPLE_BYTES;
    if (frame.sample_len > 0) {
        memcpy(frame.csi_sample, data->buf, frame.sample_len);
    }

    if (xRingbufferSendFromISR(s_csi_ringbuf, &frame, sizeof(frame), NULL) != pdTRUE) {
        s_csi_dropped_frames++;
    }
}

static void csi_consumer_task(void *param)
{
    (void)param;

    uint16_t frame_counter = 0;
    uint8_t peak_counter = 0;
    uint16_t cooldown_timer = 0;
    uint32_t total_frames_processed = 0;

    while (1) {
        size_t item_size = 0;
        csi_frame_record_t *frame = (csi_frame_record_t *)xRingbufferReceive(
            s_csi_ringbuf, &item_size, pdMS_TO_TICKS(100));

        if (frame != NULL) {
            if (item_size >= sizeof(csi_frame_record_t)) {
                uint32_t current_frame_amplitude = 0;
                uint16_t feature_bytes = frame->sample_len < CSI_FEATURE_BYTES
                                             ? frame->sample_len
                                             : CSI_FEATURE_BYTES;

                feature_bytes &= (uint16_t)~1U;
                for (uint16_t i = 0; i < feature_bytes; i += 2) {
                    int imag = frame->csi_sample[i];
                    int real = frame->csi_sample[i + 1];
                    current_frame_amplitude += (uint32_t)(abs(imag) + abs(real));
                }

                window_sum -= moving_window[window_index];
                window_sum += current_frame_amplitude;
                moving_window[window_index] = current_frame_amplitude;
                window_index = (uint8_t)((window_index + 1U) % WINDOW_SIZE);

                uint32_t moving_average = window_sum / WINDOW_SIZE;
                uint32_t deviation = current_frame_amplitude > moving_average
                                         ? current_frame_amplitude - moving_average
                                         : moving_average - current_frame_amplitude;

                total_frames_processed++;
                if (total_frames_processed > WINDOW_SIZE) {
                    if (cooldown_timer > 0) {
                        cooldown_timer--;
                    } else {
                        if (deviation > DEV_THRESHOLD) {
                            peak_counter++;
                        }

                        frame_counter++;

                        if (peak_counter >= PEAK_COUNT_TRIGGER) {
                            peak_counter = 0;
                            frame_counter = 0;
                            cooldown_timer = COOLDOWN_FRAMES;
                        } else if (frame_counter >= TIME_WINDOW_FRAMES) {
                            peak_counter = 0;
                            frame_counter = 0;
                        }
                    }
                    
                    ei_inference_add_frame((float)current_frame_amplitude, (float)moving_average, (float)deviation);
                }
            }
            vRingbufferReturnItem(s_csi_ringbuf, frame);
        }
    }
}

esp_err_t csi_handler_start(void)
{
    ei_inference_init();

    if (s_csi_ringbuf == NULL) {
        s_csi_ringbuf = xRingbufferCreateStatic(CSI_RINGBUF_STORAGE_SIZE,
                                                RINGBUF_TYPE_NOSPLIT,
                                                s_csi_ringbuf_storage,
                                                &s_csi_ringbuf_struct);
        ESP_RETURN_ON_FALSE(s_csi_ringbuf != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create CSI ring buffer");
    }

    if (s_csi_consumer_task == NULL) {
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(csi_consumer_task,
                                                        "csi_consumer",
                                                        CSI_CONSUMER_STACK_SIZE,
                                                        NULL,
                                                        CSI_CONSUMER_PRIORITY,
                                                        &s_csi_consumer_task,
                                                        CSI_CONSUMER_CORE_ID,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                            "Failed to create CSI consumer task");
    }

    if (s_csi_enabled) {
        return ESP_OK;
    }

    const wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL), TAG,
                        "Failed to register CSI callback");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi_config(&csi_config), TAG,
                        "Failed to configure CSI");
    ESP_RETURN_ON_ERROR(esp_wifi_set_csi(true), TAG,
                        "Failed to enable CSI");

    s_csi_dropped_frames = 0;
    s_csi_enabled = true;
    ESP_LOGI(TAG, "CSI capture enabled");
    return ESP_OK;
}

void csi_handler_stop(void)
{
    if (!s_csi_enabled) {
        return;
    }

    esp_err_t err = esp_wifi_set_csi(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable CSI: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_csi_rx_cb(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear CSI callback: %s", esp_err_to_name(err));
    }

    s_csi_enabled = false;
    ESP_LOGI(TAG, "CSI capture disabled");
}
