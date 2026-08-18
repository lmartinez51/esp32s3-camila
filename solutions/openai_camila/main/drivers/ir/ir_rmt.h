/**
 * @file ir_rmt.h
 * @brief IR RMT engine, learned-code store and shared driver logic (Phase 5).
 *
 * The engine owns one RMT TX channel (38/40 kHz carrier, 1 MHz resolution)
 * and one RMT RX channel (idle-gated capture) for LEARN_IR_CODE. Pulse
 * trains are transmitted from a dedicated PSRAM-stack task (non-blocking);
 * learning is a bounded synchronous capture with a decoded result.
 *
 * IR devices have no physical probe: probe() reports present=true (virtual
 * presence) — IR emitters are always "reachable".
 *
 * Learned codes are kept in a RAM cache and persisted to NVS namespace
 * `ir_codes` under keys `lr_<crc32>` by a dedicated worker task (INTERNAL
 * SRAM stack, core 0, serialized with the legacy nvs mutex). Legacy blobs
 * that still live in `robot_registry` (v1) are migrated one-shot at engine
 * boot. Erase APIs (ir_learn_delete / ir_learn_delete_all) evict the RAM
 * cache and queue the NVS removal on the same worker.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef IR_RMT_H
#define IR_RMT_H

#include "esp_err.h"
#include "robot_types.h"
#include "robot_hal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Raw pulse representation ────────────────────────────────────────── */

typedef struct
{
    uint32_t duration_us;
    bool     level; /* true = mark (carrier on), false = space */
} ir_pulse_t;

#define IR_LEARN_MAX_PULSES 256u

/* ── Decoded IR frame ────────────────────────────────────────────────── */

typedef struct
{
    bool     valid;
    robot_ir_protocol_t protocol;
    uint32_t address;
    uint32_t command;
} ir_decoded_t;

/* ── Codec descriptor (implemented by ir_nec/ir_sony/ir_rc5) ─────────── */

typedef struct ir_codec ir_codec_t;

struct ir_codec
{
    const char *profile_id;
    const char *name;
    uint32_t carrier_hz;
    /* Encode address+command into pulses (levels alternate starting with
     * mark). Returns ESP_OK and *out_len; ESP_ERR_NO_MEM / INVALID_ARG. */
    esp_err_t (*encode)(uint32_t address, uint32_t command,
                        ir_pulse_t *pulses, uint16_t max_pulses,
                        uint16_t *out_len);
    /* Decode pulses into address/command. Sets decoded.valid=false when
     * the train does not match the protocol. */
    esp_err_t (*decode)(const ir_pulse_t *pulses, uint16_t len,
                        ir_decoded_t *decoded);
};

/* ── Engine lifecycle ────────────────────────────────────────────────── */

/**
 * @brief Lightweight engine registration. The RMT channels (GDMA),
 * semaphores, learn queue and NVS worker are created LAZILY on the first
 * real use (send/capture/learn) — a base BOX-3 without the IR sensor dock
 * reserves no internal SRAM/DMA for the IR subsystem. Idempotent; safe to
 * call at boot from app startup.
 */
esp_err_t ir_rmt_init(void);

/**
 * @brief Send a pulse train (non-blocking: queued to a PSRAM-stack task).
 * @param pulses     Pulse train (copied before returning).
 * @param len        Number of pulses.
 * @param carrier_hz Carrier frequency (NEC/RC5: 38000, SONY: 40000).
 */
esp_err_t ir_rmt_send_pulses(const ir_pulse_t *pulses, uint16_t len,
                             uint32_t carrier_hz);

/**
 * @brief Capture a single IR frame (blocking, bounded by timeout_ms).
 * Arms the RX channel and waits for the idle gap. On timeout returns
 * ESP_ERR_TIMEOUT and *out_len = 0.
 */
esp_err_t ir_rmt_capture_pulses(ir_pulse_t *pulses, uint16_t max_pulses,
                                uint16_t *out_len, uint32_t timeout_ms);

/**
 * @brief Shared probe: IR is a virtual link — always present.
 */
esp_err_t ir_rmt_probe(robot_driver_t *drv, const robot_endpoint_t *ep,
                       bool *present);

/**
 * @brief Shared execute: routes SEND_IR_COMMAND / LEARN_IR_CODE for any
 * codec driver (codec taken from drv->priv).
 */
esp_err_t ir_rmt_codec_execute(robot_driver_t *drv, const char *alias,
                               robot_action_id_t action,
                               const robot_action_params_t *params,
                               robot_result_t *out);

/* ── Learned-code store (RAM cache + NVS) ────────────────────────────── */

/**
 * @brief Save a learned code for a device (RAM cache + async NVS).
 * @param id      Device id (CRC32 of endpoint).
 * @param pulses  Raw captured train (copied to PSRAM cache).
 * @param len     Number of pulses.
 * @param decoded Decoded info (may have valid=false).
 */
esp_err_t ir_learn_save(uint32_t id, const ir_pulse_t *pulses, uint16_t len,
                        const ir_decoded_t *decoded);

/**
 * @brief Look up the learned code of a device.
 * @return Pointer into the cache (valid until next save/load) or NULL.
 */
const ir_decoded_t *ir_learn_get(uint32_t id, const ir_pulse_t **pulses,
                                 uint16_t *len);

/**
 * @brief Delete one learned code (RAM cache + async NVS erase).
 * Safe to call before the engine ever boots (nothing persisted, no-op).
 * @param id Device id (CRC32 of endpoint).
 */
esp_err_t ir_learn_delete(uint32_t id);

/**
 * @brief Delete every learned code (RAM cache + async NVS wipe).
 * Safe to call before the engine ever boots (no-op).
 */
esp_err_t ir_learn_delete_all(void);

#ifdef __cplusplus
}
#endif

#endif /* IR_RMT_H */
