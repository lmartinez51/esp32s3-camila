/**
 * @file ir_rc5.c
 * @brief Philips RC5 IR codec (Phase 5): encode/decode implementation.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ir_rc5.h"

#include <string.h>
#include "esp_log.h"

#define TAG "IR_RC5"

#define IR_RC5_CARRIER_HZ       36000u
#define IR_RC5_HALF_US          889u
#define IR_RC5_BITS             14u
#define IR_RC5_START_BITS       2u
#define IR_RC5_ADDR_BITS        5u
#define IR_RC5_CMD_BITS         6u
#define IR_RC5_PULSES_MIN       26u /* bit final en 0 funde su mitad B en el idle */
#define IR_RC5_PULSES_MAX       27u /* 2 * 14 halves - 1 (idle half omitido) */
#define IR_RC5_TOLERANCE_PCT    30u

static bool ir_rc5_dur_ok(uint32_t dur)
{
    const uint32_t tol = IR_RC5_HALF_US * IR_RC5_TOLERANCE_PCT / 100;
    return dur >= IR_RC5_HALF_US - tol && dur <= IR_RC5_HALF_US + tol;
}

static esp_err_t ir_rc5_encode(uint32_t address, uint32_t command,
                               ir_pulse_t *pulses, uint16_t max_pulses,
                               uint16_t *out_len)
{
    if (pulses == NULL || out_len == NULL || max_pulses < IR_RC5_PULSES_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (address > 0x1F || command > 0x3F)
    {
        ESP_LOGW(TAG, "encode: address (0-31) / command (0-63) fuera de rango RC5");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t i = 0;

    /* Bit 0 (start bit 1, siempre 1): la mitad A (espacio) se funde con el
     * idle previo; la trama arranca con la mitad B (mark). */
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_RC5_HALF_US, .level = true };

    for (uint32_t b = 1; b < IR_RC5_BITS; b++)
    {
        uint32_t value = 0;
        if (b < IR_RC5_START_BITS)
        {
            value = 1; /* start bit 2 */
        }
        else if (b == IR_RC5_START_BITS)
        {
            value = 0; /* toggle: siempre 0 en el emisor */
        }
        else if (b < IR_RC5_START_BITS + 1 + IR_RC5_ADDR_BITS)
        {
            const uint32_t idx = b - (IR_RC5_START_BITS + 1);
            value = (address >> (IR_RC5_ADDR_BITS - 1 - idx)) & 1u; /* MSB first */
        }
        else
        {
            const uint32_t idx = b - (IR_RC5_START_BITS + 1 + IR_RC5_ADDR_BITS);
            value = (command >> (IR_RC5_CMD_BITS - 1 - idx)) & 1u; /* MSB first */
        }

        pulses[i++] = (ir_pulse_t){ .duration_us = IR_RC5_HALF_US,
                                    .level = (value == 0) };
        pulses[i++] = (ir_pulse_t){ .duration_us = IR_RC5_HALF_US,
                                    .level = (value == 1) };
    }

    *out_len = i;
    return ESP_OK;
}

static esp_err_t ir_rc5_decode(const ir_pulse_t *pulses, uint16_t len,
                               ir_decoded_t *decoded)
{
    if (pulses == NULL || decoded == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(decoded, 0, sizeof(*decoded));

    if (len < IR_RC5_PULSES_MIN)
    {
        return ESP_ERR_NOT_FOUND;
    }

    for (uint16_t i = 0; i < IR_RC5_PULSES_MIN; i++)
    {
        if (!ir_rc5_dur_ok(pulses[i].duration_us))
        {
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Valor de cada bit = nivel de su mitad B. El bit 0 usa el pulso 0
     * (su mitad A se omitio); para bit b >= 1 la mitad B es el pulso 2*b.
     * Con len == 26 el bit 13 es 0: su mitad B (espacio) se fundio con el
     * idle y la trama termina con la mitad A (mark). */
    uint32_t bits[IR_RC5_BITS] = {0};
    bits[0] = pulses[0].level ? 1u : 0u;
    for (uint32_t b = 1; b < IR_RC5_BITS; b++)
    {
        const uint32_t b_half_idx = 2 * b;
        if (b_half_idx >= len)
        {
            bits[b] = 0;
            break; /* solo puede ocurrir en el bit final */
        }
        bits[b] = pulses[b_half_idx].level ? 1u : 0u;
        const uint32_t a_level = pulses[b_half_idx - 1].level ? 1u : 0u;
        if (a_level == bits[b])
        {
            return ESP_ERR_NOT_FOUND; /* las mitades A y B deben ser complementarias */
        }
    }

    if (bits[0] != 1 || bits[1] != 1)
    {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t address = 0;
    for (uint32_t k = 0; k < IR_RC5_ADDR_BITS; k++)
    {
        address |= bits[IR_RC5_START_BITS + 1 + k] << (IR_RC5_ADDR_BITS - 1 - k);
    }
    uint32_t command = 0;
    for (uint32_t k = 0; k < IR_RC5_CMD_BITS; k++)
    {
        command |= bits[IR_RC5_START_BITS + 1 + IR_RC5_ADDR_BITS + k]
                   << (IR_RC5_CMD_BITS - 1 - k);
    }

    decoded->valid = true;
    decoded->protocol = ROBOT_IR_PROTOCOL_RC5;
    decoded->address = address;
    decoded->command = command;
    return ESP_OK;
}

const ir_codec_t ir_rc5_codec = {
    .profile_id = IR_RC5_PROFILE_ID,
    .name = "RC5",
    .carrier_hz = IR_RC5_CARRIER_HZ,
    .encode = ir_rc5_encode,
    .decode = ir_rc5_decode,
};

static robot_driver_t s_drv;

const robot_driver_t *ir_rc5_get_driver(void)
{
    if (s_drv.profile_id[0] == '\0')
    {
        memset(&s_drv, 0, sizeof(s_drv));
        strncpy(s_drv.profile_id, IR_RC5_PROFILE_ID, sizeof(s_drv.profile_id) - 1);
        s_drv.category = ROBOT_CATEGORY_IR_ACTUATOR;
        s_drv.protocol = ROBOT_PROTOCOL_IR;
        s_drv.capabilities = (1u << ROBOT_ACTION_SEND_IR_COMMAND)
                             | (1u << ROBOT_ACTION_LEARN_IR_CODE);
        s_drv.execute = ir_rmt_codec_execute;
        s_drv.probe = ir_rmt_probe;
        s_drv.priv = (void *)&ir_rc5_codec;
    }
    return &s_drv;
}
