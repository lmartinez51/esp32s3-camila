#ifndef EI_INFERENCE_H
#define EI_INFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool motion_active;
    bool resting;
    uint32_t updated_ms;
    float corr_drop;
    float phase_motion_energy;
} ei_inference_status_t;

// Initialize/reset the CSI DSP motion trigger.
void ei_inference_init(void);

// Feed one raw CSI frame through the DSP pre-processing layer.
void ei_inference_add_csi_frame(const int8_t *csi_sample,
                                uint16_t sample_len,
                                bool first_word_invalid);

void ei_inference_get_status(ei_inference_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // EI_INFERENCE_H
