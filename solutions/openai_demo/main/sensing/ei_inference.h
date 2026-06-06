#ifndef EI_INFERENCE_H
#define EI_INFERENCE_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the FreeRTOS inference task
void ei_inference_init(void);

// Thread-safe mechanism to feed data into the double-buffer
void ei_inference_add_frame(float amp, float avg, float dev);

#ifdef __cplusplus
}
#endif

#endif // EI_INFERENCE_H
