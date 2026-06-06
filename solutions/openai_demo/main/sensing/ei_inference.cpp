#include "ei_inference.h"
#include "app_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

// Configuration
#define NUM_FRAMES 50
#define FRAME_SIZE 3 // [Amp, Avg, Dev]
#define BUFFER_SIZE (NUM_FRAMES * FRAME_SIZE)
#define EI_INFERENCE_STACK_SIZE 8192
#define EI_INFERENCE_PRIORITY (tskIDLE_PRIORITY + 8)
#define EI_INFERENCE_CORE_ID 1
#define DEV_BASELINE_CALIBRATION_WINDOWS 12
#define DEV_BASELINE_ALPHA 0.03f
#define DEV_TRIGGER_MARGIN 5.0f
#define DEV_CLEAR_MARGIN 2.0f
#define DEV_TRIGGER_WINDOWS 3
#define DEV_CLEAR_WINDOWS 4

// Double Buffering System (Static allocation, 0 heap fragmentation)
static float inference_buffer_1[BUFFER_SIZE];
static float inference_buffer_2[BUFFER_SIZE];
static float *current_write_buffer = inference_buffer_1;
static float *current_read_buffer = NULL;

static volatile int buffer_index = 0;
static TaskHandle_t inference_task_handle = NULL;

// FreeRTOS Inference Task pinned to Core 1
static void inference_task(void *pvParameters)
{
    int movement_windows = 0;
    int clear_windows = 0;
    int calibration_windows = 0;
    float baseline_dev = 0.0f;
    bool baseline_ready = false;
    bool alarm_latched = false;

    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (current_read_buffer == NULL)
            continue;

        float dev_sum = 0.0f;
        for (int frame = 0; frame < NUM_FRAMES; frame++)
        {
            dev_sum += current_read_buffer[(frame * FRAME_SIZE) + 2];
        }

        float avg_dev = dev_sum / NUM_FRAMES;

        if (!baseline_ready)
        {
            baseline_dev += avg_dev;
            calibration_windows++;

            if (calibration_windows >= DEV_BASELINE_CALIBRATION_WINDOWS)
            {
                baseline_dev /= DEV_BASELINE_CALIBRATION_WINDOWS;
                baseline_ready = true;
            }

            continue;
        }

        float trigger_threshold = baseline_dev + DEV_TRIGGER_MARGIN;
        float clear_threshold = baseline_dev + DEV_CLEAR_MARGIN;

        if (avg_dev >= trigger_threshold)
        {
            if (movement_windows < DEV_TRIGGER_WINDOWS)
            {
                movement_windows++;
            }

            clear_windows = 0;

            if (!alarm_latched && movement_windows >= DEV_TRIGGER_WINDOWS)
            {
                ei_printf("\n[WARNING] INTRUDER DETECTED!\n");
                orchestrator_post_event(ORCH_EVENT_MOTION_DETECTED);
                alarm_latched = true;
            }
        }
        else if (avg_dev <= clear_threshold)
        {
            clear_windows++;

            if (clear_windows >= DEV_CLEAR_WINDOWS)
            {
                movement_windows = 0;
                alarm_latched = false;

                baseline_dev = (baseline_dev * (1.0f - DEV_BASELINE_ALPHA)) +
                               (avg_dev * DEV_BASELINE_ALPHA);
            }
            else
            {
                if (movement_windows == 0 && !alarm_latched)
                {
                    baseline_dev = (baseline_dev * (1.0f - DEV_BASELINE_ALPHA)) +
                                   (avg_dev * DEV_BASELINE_ALPHA);
                }
            }
        }
        else
        {
            clear_windows = 0;

            if (movement_windows == 0 && !alarm_latched)
            {
                baseline_dev = (baseline_dev * (1.0f - DEV_BASELINE_ALPHA)) +
                               (avg_dev * DEV_BASELINE_ALPHA);
            }
        }
    }
}

// -------------------------------------------------------------------------
// C-API EXPORTS
// -------------------------------------------------------------------------

extern "C" void ei_inference_init(void)
{
    if (inference_task_handle == NULL)
    {
        xTaskCreatePinnedToCore(
            inference_task,
            "inference_task",
            EI_INFERENCE_STACK_SIZE,
            NULL,
            EI_INFERENCE_PRIORITY,
            &inference_task_handle,
            EI_INFERENCE_CORE_ID);
    }
}

extern "C" void ei_inference_add_frame(float amp, float avg, float dev)
{
    if (buffer_index < BUFFER_SIZE)
    {
        current_write_buffer[buffer_index++] = amp;
        current_write_buffer[buffer_index++] = avg;
        current_write_buffer[buffer_index++] = dev;
    }

    // When 50 frames (150 floats) are collected:
    if (buffer_index >= BUFFER_SIZE)
    {
        // 1. Thread-safe buffer swap
        current_read_buffer = current_write_buffer;
        current_write_buffer = (current_write_buffer == inference_buffer_1) ? inference_buffer_2 : inference_buffer_1;

        // 2. Reset index for the next 50 frames
        buffer_index = 0;

        // 3. Wake up the inference task
        if (inference_task_handle != NULL)
        {
            xTaskNotifyGive(inference_task_handle);
        }
    }
}
