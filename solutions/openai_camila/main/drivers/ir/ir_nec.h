/**
 * @file ir_nec.h
 * @brief NEC IR codec (Phase 5): encode/decode + driver for IR NEC devices.
 *
 * NEC frame: 9 ms mark + 4.5 ms space header, then 32 bits LSB-first
 * (address, ~address, command, ~command), bit 0 = 562/562 us,
 * bit 1 = 562/1687 us, closing 562 us mark. Carrier 38 kHz.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef IR_NEC_H
#define IR_NEC_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"
#include "ir_rmt.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define IR_NEC_PROFILE_ID "ir_nec"

extern const ir_codec_t ir_nec_codec;

/**
 * @brief Get the ir_nec driver descriptor (registered by device_registry).
 */
const robot_driver_t *ir_nec_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* IR_NEC_H */
