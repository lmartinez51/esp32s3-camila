/**
 * @file ir_nec.c
 * @brief NEC IR codec (Phase 5): encode/decode + driver implementation.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ir_nec.h"

#include <string.h>
#include "esp_log.h"

#define TAG "IR_NEC"

#define IR_NEC_CARRIER_HZ       38000u
#define IR_NEC_HEADER_MARK      9000u
#define IR_NEC_HEADER_SPACE     4500u
#define IR_NEC_BIT_MARK         562u
#define IR_NEC_BIT_ZERO_SPACE   562u
#define IR_NEC_BIT_ONE_SPACE    1687u
#define IR_NEC_PULSES_MAX       67u /* header 2 + 32 bits + stop mark */
#define IR_NEC_TOLERANCE_PCT    30u

static bool ir_nec_dur_ok(uint32_t dur, uint32_t expected)
{
    const uint32_t tol = expected * IR_NEC_TOLERANCE_PCT / 100;
    return dur >= expected - tol && dur <= expected + tol;
}

static esp_err_t ir_nec_encode(uint32_t address, uint32_t command,
                               ir_pulse_t *pulses, uint16_t max_pulses,
                               uint16_t *out_len)
{
    if (pulses == NULL || out_len == NULL || max_pulses < IR_NEC_PULSES_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (address > 0xFF || command > 0xFF)
    {
        ESP_LOGW(TAG, "encode: address/command fuera de rango NEC (0-255)");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t addr = (uint8_t)address;
    const uint8_t cmd = (uint8_t)command;
    const uint32_t bits = (uint32_t)addr
                          | ((uint32_t)(~addr & 0xFF) << 8)
                          | ((uint32_t)cmd << 16)
                          | ((uint32_t)(~cmd & 0xFF) << 24);

    uint16_t i = 0;
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_NEC_HEADER_MARK, .level = true };
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_NEC_HEADER_SPACE, .level = false };
    for (uint32_t m = 0; m < 32; m++)
    {
        pulses[i++] = (ir_pulse_t){ .duration_us = IR_NEC_BIT_MARK, .level = true };
        const bool one = ((bits >> m) & 1u) != 0;
        pulses[i++] = (ir_pulse_t){ .duration_us = one ? IR_NEC_BIT_ONE_SPACE : IR_NEC_BIT_ZERO_SPACE,
                                    .level = false };
    }
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_NEC_BIT_MARK, .level = true };

    *out_len = i;
    return ESP_OK;
}

static esp_err_t ir_nec_decode(const ir_pulse_t *pulses, uint16_t len,
                               ir_decoded_t *decoded)
{
    if (pulses == NULL || decoded == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(decoded, 0, sizeof(*decoded));

    if (len < IR_NEC_PULSES_MAX)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (!pulses[0].level || !ir_nec_dur_ok(pulses[0].duration_us, IR_NEC_HEADER_MARK))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (pulses[1].level || !ir_nec_dur_ok(pulses[1].duration_us, IR_NEC_HEADER_SPACE))
    {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t bits = 0;
    uint16_t i = 2;
    for (uint32_t m = 0; m < 32; m++)
    {
        if (i + 1 >= len)
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (!pulses[i].level || !ir_nec_dur_ok(pulses[i].duration_us, IR_NEC_BIT_MARK))
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (pulses[i + 1].level)
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (ir_nec_dur_ok(pulses[i + 1].duration_us, IR_NEC_BIT_ONE_SPACE))
        {
            bits |= (1u << m);
        }
        else if (!ir_nec_dur_ok(pulses[i + 1].duration_us, IR_NEC_BIT_ZERO_SPACE))
        {
            return ESP_ERR_NOT_FOUND;
        }
        i += 2;
    }

    if (i >= len || !pulses[i].level ||
        !ir_nec_dur_ok(pulses[i].duration_us, IR_NEC_BIT_MARK))
    {
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t addr = (uint8_t)(bits & 0xFF);
    const uint8_t naddr = (uint8_t)((bits >> 8) & 0xFF);
    const uint8_t cmd = (uint8_t)((bits >> 16) & 0xFF);
    const uint8_t ncmd = (uint8_t)((bits >> 24) & 0xFF);
    if (addr != (uint8_t)~naddr || cmd != (uint8_t)~ncmd)
    {
        return ESP_ERR_NOT_FOUND;
    }

    decoded->valid = true;
    decoded->protocol = ROBOT_IR_PROTOCOL_NEC;
    decoded->address = addr;
    decoded->command = cmd;
    return ESP_OK;
}

const ir_codec_t ir_nec_codec = {
    .profile_id = IR_NEC_PROFILE_ID,
    .name = "NEC",
    .carrier_hz = IR_NEC_CARRIER_HZ,
    .encode = ir_nec_encode,
    .decode = ir_nec_decode,
};

static robot_driver_t s_drv;

const robot_driver_t *ir_nec_get_driver(void)
{
    if (s_drv.profile_id[0] == '\0')
    {
        memset(&s_drv, 0, sizeof(s_drv));
        strncpy(s_drv.profile_id, IR_NEC_PROFILE_ID, sizeof(s_drv.profile_id) - 1);
        s_drv.category = ROBOT_CATEGORY_IR_ACTUATOR;
        s_drv.protocol = ROBOT_PROTOCOL_IR;
        s_drv.capabilities = (1u << ROBOT_ACTION_SEND_IR_COMMAND)
                             | (1u << ROBOT_ACTION_LEARN_IR_CODE);
        s_drv.execute = ir_rmt_codec_execute;
        s_drv.probe = ir_rmt_probe;
        s_drv.priv = (void *)&ir_nec_codec;
    }
    return &s_drv;
}
