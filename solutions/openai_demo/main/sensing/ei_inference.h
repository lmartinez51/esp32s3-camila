#ifndef EI_INFERENCE_H
#define EI_INFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize/reset the CSI DSP motion trigger.
void ei_inference_init(void);

// Feed one raw CSI frame through the DSP pre-processing layer.
void ei_inference_add_csi_frame(const int8_t *csi_sample,
                                uint16_t sample_len,
                                bool first_word_invalid);

#ifdef __cplusplus
}
#endif

#endif // EI_INFERENCE_H
