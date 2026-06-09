#include "ei_inference.h"

#include "app_events.h"
#include "csi_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define CSI_DSP_CORR_DROP_TRIGGER 0.4f
#define CSI_DSP_PHASE_ENERGY_TRIGGER 1.0f
#define CSI_DSP_CORR_DROP_CLEAR 0.10f
#define CSI_DSP_PHASE_ENERGY_CLEAR 0.25f
#define CSI_DSP_CLEAR_FRAMES 8
#define CSI_DSP_TELEMETRY_INTERVAL_FRAMES 50

static const char *TAG = "CSI_DSP";

static csi_dsp_state_t csi_dsp_state;
static bool motion_latched = false;
static uint8_t clear_frame_count = 0;
static uint32_t csi_dsp_telemetry_counter = 0;
static ei_inference_status_t latest_status;
static portMUX_TYPE latest_status_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ei_inference_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void ei_inference_update_status(const csi_dsp_features_t *features,
                                       bool motion_active,
                                       bool resting)
{
    if (!features)
    {
        return;
    }

    ei_inference_status_t status = {
        .valid = true,
        .motion_active = motion_active,
        .resting = resting,
        .updated_ms = ei_inference_now_ms(),
        .corr_drop = features->corr_drop,
        .phase_motion_energy = features->phase_motion_energy,
    };

    portENTER_CRITICAL(&latest_status_mux);
    latest_status = status;
    portEXIT_CRITICAL(&latest_status_mux);
}

static void ei_inference_reset_state(void)
{
    csi_dsp_reset(&csi_dsp_state);
    motion_latched = false;
    clear_frame_count = 0;
    csi_dsp_telemetry_counter = 0;

    portENTER_CRITICAL(&latest_status_mux);
    latest_status = {};
    portEXIT_CRITICAL(&latest_status_mux);
}

extern "C" void ei_inference_init(void)
{
    ei_inference_reset_state();
}

extern "C" void ei_inference_add_csi_frame(const int8_t *csi_sample,
                                           uint16_t sample_len,
                                           bool first_word_invalid)
{
    csi_dsp_features_t features;
    if (!csi_dsp_process_frame(&csi_dsp_state,
                               csi_sample,
                               sample_len,
                               first_word_invalid,
                               &features))
    {
        return;
    }

    bool motion_detected = (features.corr_drop > CSI_DSP_CORR_DROP_TRIGGER) ||
                           (features.phase_motion_energy > CSI_DSP_PHASE_ENERGY_TRIGGER);
    bool resting = (features.corr_drop < CSI_DSP_CORR_DROP_CLEAR) &&
                   (features.phase_motion_energy < CSI_DSP_PHASE_ENERGY_CLEAR);

    if (motion_detected)
    {
        clear_frame_count = 0;

        if (!motion_latched)
        {
            motion_latched = true;
            ESP_LOGW(TAG,
                     "DSP motion detected: corr_drop=%.4f phase_energy=%.4f amp_energy=%.2f carriers=%u",
                     features.corr_drop,
                     features.phase_motion_energy,
                     features.amp_motion_energy,
                     (unsigned int)features.usable_subcarriers);
            orchestrator_post_motion_detected(ei_inference_now_ms(), features.corr_drop);
        }
    }
    else if (resting)
    {
        if (clear_frame_count < CSI_DSP_CLEAR_FRAMES)
        {
            clear_frame_count++;
        }

        if (clear_frame_count >= CSI_DSP_CLEAR_FRAMES)
        {
            motion_latched = false;
        }
    }
    else
    {
        clear_frame_count = 0;
    }

    ei_inference_update_status(&features, motion_latched, resting);

    csi_dsp_telemetry_counter++;
    if (csi_dsp_telemetry_counter >= CSI_DSP_TELEMETRY_INTERVAL_FRAMES)
    {
        csi_dsp_telemetry_counter = 0;
        ESP_LOGD(TAG,
                 "amp=%.2f amp_energy=%.2f phase_energy=%.4f corr_drop=%.4f carriers=%u slope=%.4f offset=%.4f latched=%d",
                 features.filtered_amplitude,
                 features.amp_motion_energy,
                 features.phase_motion_energy,
                 features.corr_drop,
                 (unsigned int)features.usable_subcarriers,
                 features.phase_slope,
                 features.phase_offset,
                 motion_latched);
    }
}

extern "C" void ei_inference_get_status(ei_inference_status_t *status)
{
    if (!status)
    {
        return;
    }

    portENTER_CRITICAL(&latest_status_mux);
    *status = latest_status;
    portEXIT_CRITICAL(&latest_status_mux);
}
