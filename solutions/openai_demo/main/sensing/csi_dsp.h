#ifndef CSI_DSP_H
#define CSI_DSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_DSP_COMPLEX_PAIRS 64
#define CSI_DSP_SAMPLE_BYTES (CSI_DSP_COMPLEX_PAIRS * 2)

typedef struct {
    bool valid;
    uint8_t usable_subcarriers;
    float filtered_amplitude;
    float amp_motion_energy;
    float phase_motion_energy;
    float corr_drop;
    float phase_slope;
    float phase_offset;
} csi_dsp_features_t;

typedef struct {
    bool initialized;
    uint32_t frames_processed;
    bool carrier_initialized[CSI_DSP_COMPLEX_PAIRS];
    float amp_lp[CSI_DSP_COMPLEX_PAIRS];
    float prev_amp[CSI_DSP_COMPLEX_PAIRS];
    float prev_phase[CSI_DSP_COMPLEX_PAIRS];

    int8_t temp_k[CSI_DSP_COMPLEX_PAIRS];
    uint8_t temp_bin[CSI_DSP_COMPLEX_PAIRS];
    float temp_amp[CSI_DSP_COMPLEX_PAIRS];
    float temp_phase[CSI_DSP_COMPLEX_PAIRS];
    float temp_weight[CSI_DSP_COMPLEX_PAIRS];
} csi_dsp_state_t;

void csi_dsp_init(csi_dsp_state_t *state);
void csi_dsp_reset(csi_dsp_state_t *state);

bool csi_dsp_process_frame(csi_dsp_state_t *state,
                           const int8_t *csi_sample,
                           uint16_t sample_len,
                           bool first_word_invalid,
                           csi_dsp_features_t *features);

#ifdef __cplusplus
}
#endif

#endif // CSI_DSP_H
