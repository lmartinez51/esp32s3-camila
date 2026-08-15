/**
 * @file ir_rc5.h
 * @brief Philips RC5 IR codec (Phase 5): encode/decode + driver.
 *
 * RC5 frame: 14 bits of 1778 us (2 x 889 us halves), the bit value is
 * carried by the level of the second half (1 = mark). Two start bits (1,1),
 * toggle bit, 5 address bits MSB-first, 6 command bits MSB-first. The
 * leading idle-space half of start bit 1 is omitted: frames begin with an
 * 889 us mark. Carrier 36 kHz.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef IR_RC5_H
#define IR_RC5_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"
#include "ir_rmt.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define IR_RC5_PROFILE_ID "ir_rc5"

extern const ir_codec_t ir_rc5_codec;

/**
 * @brief Get the ir_rc5 driver descriptor (registered by device_registry).
 */
const robot_driver_t *ir_rc5_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* IR_RC5_H */
