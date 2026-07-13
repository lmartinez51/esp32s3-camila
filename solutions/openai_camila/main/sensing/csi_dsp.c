#include "csi_dsp.h"

#include <math.h>
#include <string.h>

#define CSI_DSP_PI 3.14159265358979323846f
#define CSI_DSP_TWO_PI (2.0f * CSI_DSP_PI)
#define CSI_DSP_MIN_USABLE_SUBCARRIERS 8
#define CSI_DSP_AMP_ALPHA 0.12f
#define CSI_DSP_EPSILON 1.0e-6f

static float csi_dsp_absf(float value)
{
    return value < 0.0f ? -value : value;
}

static float csi_dsp_clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float csi_dsp_wrap_pi(float phase)
{
    while (phase > CSI_DSP_PI) {
        phase -= CSI_DSP_TWO_PI;
    }
    while (phase < -CSI_DSP_PI) {
        phase += CSI_DSP_TWO_PI;
    }
    return phase;
}

static bool csi_dsp_static_subcarrier_allowed(int8_t k)
{
    if (k == 0) {
        return false;
    }

    return csi_dsp_absf((float)k) <= 28.0f;
}

static uint8_t csi_dsp_subcarrier_to_pair(int8_t k)
{
    return k >= 0 ? (uint8_t)k : (uint8_t)(k + 64);
}

void csi_dsp_init(csi_dsp_state_t *state)
{
    csi_dsp_reset(state);
}

void csi_dsp_reset(csi_dsp_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

bool csi_dsp_process_frame(csi_dsp_state_t *state,
                           const int8_t *csi_sample,
                           uint16_t sample_len,
                           bool first_word_invalid,
                           csi_dsp_features_t *features)
{
    if (state == NULL || csi_sample == NULL || features == NULL) {
        return false;
    }

    memset(features, 0, sizeof(*features));

    uint16_t pair_count = sample_len / 2U;
    if (pair_count > CSI_DSP_COMPLEX_PAIRS) {
        pair_count = CSI_DSP_COMPLEX_PAIRS;
    }

    uint8_t usable_count = 0;
    float amp_sum = 0.0f;

    for (int8_t k = -32; k <= 31; k++) {
        uint8_t pair_index = csi_dsp_subcarrier_to_pair(k);
        if (pair_index >= pair_count) {
            continue;
        }

        if (!csi_dsp_static_subcarrier_allowed(k)) {
            continue;
        }

        if (first_word_invalid && pair_index < 2U) {
            continue;
        }

        int imag = csi_sample[(uint16_t)pair_index * 2U];
        int real = csi_sample[((uint16_t)pair_index * 2U) + 1U];

        if (real <= -126 || real >= 126 || imag <= -126 || imag >= 126) {
            continue;
        }

        float real_f = (float)real;
        float imag_f = (float)imag;
        float amp = sqrtf((real_f * real_f) + (imag_f * imag_f));

        state->temp_k[usable_count] = k;
        state->temp_bin[usable_count] = pair_index;
        state->temp_amp[usable_count] = amp;
        state->temp_phase[usable_count] = atan2f(imag_f, real_f);
        amp_sum += amp;
        usable_count++;
    }

    if (usable_count < CSI_DSP_MIN_USABLE_SUBCARRIERS) {
        return false;
    }

    float mean_amp = amp_sum / usable_count;
    if (mean_amp < CSI_DSP_EPSILON) {
        return false;
    }

    for (uint8_t i = 0; i < usable_count; i++) {
        state->temp_weight[i] = csi_dsp_clampf(state->temp_amp[i] / mean_amp, 0.25f, 4.0f);
    }

    float previous_phase = state->temp_phase[0];
    for (uint8_t i = 1; i < usable_count; i++) {
        float phase = state->temp_phase[i];
        while ((phase - previous_phase) > CSI_DSP_PI) {
            phase -= CSI_DSP_TWO_PI;
        }
        while ((phase - previous_phase) < -CSI_DSP_PI) {
            phase += CSI_DSP_TWO_PI;
        }
        state->temp_phase[i] = phase;
        previous_phase = phase;
    }

    float weight_sum = 0.0f;
    float weighted_k_sum = 0.0f;
    float weighted_phase_sum = 0.0f;

    for (uint8_t i = 0; i < usable_count; i++) {
        float weight = state->temp_weight[i];
        weight_sum += weight;
        weighted_k_sum += weight * (float)state->temp_k[i];
        weighted_phase_sum += weight * state->temp_phase[i];
    }

    if (weight_sum < CSI_DSP_EPSILON) {
        return false;
    }

    float k_mean = weighted_k_sum / weight_sum;
    float phase_mean = weighted_phase_sum / weight_sum;
    float slope_num = 0.0f;
    float slope_den = 0.0f;

    for (uint8_t i = 0; i < usable_count; i++) {
        float k_delta = (float)state->temp_k[i] - k_mean;
        float phase_delta = state->temp_phase[i] - phase_mean;
        float weight = state->temp_weight[i];
        slope_num += weight * k_delta * phase_delta;
        slope_den += weight * k_delta * k_delta;
    }

    float slope = 0.0f;
    if (slope_den > CSI_DSP_EPSILON) {
        slope = slope_num / slope_den;
    }
    float offset = phase_mean - (slope * k_mean);

    float amp_energy_sum = 0.0f;
    float phase_energy_sum = 0.0f;
    float corr_re = 0.0f;
    float corr_im = 0.0f;
    float corr_curr_norm = 0.0f;
    float corr_prev_norm = 0.0f;
    float feature_weight_sum = 0.0f;
    uint8_t corr_count = 0;

    for (uint8_t i = 0; i < usable_count; i++) {
        uint8_t bin = state->temp_bin[i];
        float amp = state->temp_amp[i];
        float weight = state->temp_weight[i];
        float clean_phase = csi_dsp_wrap_pi(
            state->temp_phase[i] - (slope * (float)state->temp_k[i]) - offset);

        if (!state->initialized || !state->carrier_initialized[bin]) {
            state->amp_lp[bin] = amp;
            state->prev_amp[bin] = amp;
            state->prev_phase[bin] = clean_phase;
            state->carrier_initialized[bin] = true;
            continue;
        }

        state->amp_lp[bin] += CSI_DSP_AMP_ALPHA * (amp - state->amp_lp[bin]);
        float amp_hp = amp - state->amp_lp[bin];
        float phase_delta = csi_dsp_wrap_pi(clean_phase - state->prev_phase[bin]);

        amp_energy_sum += weight * amp_hp * amp_hp;
        phase_energy_sum += weight * csi_dsp_absf(phase_delta);
        feature_weight_sum += weight;

        float prev_amp = state->prev_amp[bin];
        corr_re += amp * prev_amp * cosf(phase_delta);
        corr_im += amp * prev_amp * sinf(phase_delta);
        corr_curr_norm += amp * amp;
        corr_prev_norm += prev_amp * prev_amp;
        corr_count++;

        state->prev_amp[bin] = amp;
        state->prev_phase[bin] = clean_phase;
    }

    state->initialized = true;
    state->frames_processed++;

    features->valid = true;
    features->usable_subcarriers = usable_count;
    features->filtered_amplitude = amp_sum;
    features->phase_slope = slope;
    features->phase_offset = offset;

    if (feature_weight_sum > CSI_DSP_EPSILON) {
        features->amp_motion_energy = sqrtf(amp_energy_sum / feature_weight_sum);
        features->phase_motion_energy = phase_energy_sum / feature_weight_sum;
    }

    if (corr_count > 0 &&
        corr_curr_norm > CSI_DSP_EPSILON &&
        corr_prev_norm > CSI_DSP_EPSILON) {
        float corr_mag = sqrtf((corr_re * corr_re) + (corr_im * corr_im));
        float corr_norm = sqrtf((corr_curr_norm * corr_prev_norm) + CSI_DSP_EPSILON);
        float corr = csi_dsp_clampf(corr_mag / corr_norm, 0.0f, 1.0f);
        features->corr_drop = 1.0f - corr;
    }

    return true;
}
