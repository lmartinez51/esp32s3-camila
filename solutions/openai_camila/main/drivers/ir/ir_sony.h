/**
 * @file ir_sony.h
 * @brief Sony SIRC IR codec (Phase 5): 12-bit encode/decode + driver.
 *
 * SIRC 12-bit frame: 2.4 ms mark + 0.6 ms space header, then 12 bits
 * LSB-first (7 address + 5 command), bit 0 = 600/600 us, bit 1 =
 * 600/1200 us, closing 600 us mark. Carrier 40 kHz.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef IR_SONY_H
#define IR_SONY_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"
#include "ir_rmt.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define IR_SONY_PROFILE_ID "ir_sony"

extern const ir_codec_t ir_sony_codec;

/**
 * @brief Get the ir_sony driver descriptor (registered by device_registry).
 */
const robot_driver_t *ir_sony_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* IR_SONY_H */
