/**
 * @file ir_sony.c
 * @brief Sony SIRC IR codec (Phase 5): 12-bit encode/decode implementation.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ir_sony.h"

#include <string.h>
#include "esp_log.h"

#define TAG "IR_SONY"

#define IR_SONY_CARRIER_HZ       40000u
#define IR_SONY_HEADER_MARK      2400u
#define IR_SONY_HEADER_SPACE     600u
#define IR_SONY_BIT_MARK         600u
#define IR_SONY_BIT_ZERO_SPACE   600u
#define IR_SONY_BIT_ONE_SPACE    1200u
#define IR_SONY_BITS             12u
#define IR_SONY_ADDR_BITS        7u
#define IR_SONY_CMD_BITS         5u
#define IR_SONY_PULSES_MAX       27u /* header 2 + 12 bits + stop mark */
#define IR_SONY_TOLERANCE_PCT    30u

static bool ir_sony_dur_ok(uint32_t dur, uint32_t expected)
{
    const uint32_t tol = expected * IR_SONY_TOLERANCE_PCT / 100;
    return dur >= expected - tol && dur <= expected + tol;
}

static esp_err_t ir_sony_encode(uint32_t address, uint32_t command,
                                ir_pulse_t *pulses, uint16_t max_pulses,
                                uint16_t *out_len)
{
    if (pulses == NULL || out_len == NULL || max_pulses < IR_SONY_PULSES_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (address > 0x7F || command > 0x1F)
    {
        ESP_LOGW(TAG, "encode: address (0-127) / command (0-31) fuera de rango SIRC");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t bits = ((uint32_t)address & 0x7F) | ((uint32_t)command << IR_SONY_ADDR_BITS);

    uint16_t i = 0;
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_SONY_HEADER_MARK, .level = true };
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_SONY_HEADER_SPACE, .level = false };
    for (uint32_t m = 0; m < IR_SONY_BITS; m++)
    {
        pulses[i++] = (ir_pulse_t){ .duration_us = IR_SONY_BIT_MARK, .level = true };
        const bool one = ((bits >> m) & 1u) != 0;
        pulses[i++] = (ir_pulse_t){ .duration_us = one ? IR_SONY_BIT_ONE_SPACE : IR_SONY_BIT_ZERO_SPACE,
                                    .level = false };
    }
    pulses[i++] = (ir_pulse_t){ .duration_us = IR_SONY_BIT_MARK, .level = true };

    *out_len = i;
    return ESP_OK;
}

static esp_err_t ir_sony_decode(const ir_pulse_t *pulses, uint16_t len,
                                ir_decoded_t *decoded)
{
    if (pulses == NULL || decoded == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(decoded, 0, sizeof(*decoded));

    if (len < IR_SONY_PULSES_MAX)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (!pulses[0].level || !ir_sony_dur_ok(pulses[0].duration_us, IR_SONY_HEADER_MARK))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (pulses[1].level || !ir_sony_dur_ok(pulses[1].duration_us, IR_SONY_HEADER_SPACE))
    {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t bits = 0;
    uint16_t i = 2;
    for (uint32_t m = 0; m < IR_SONY_BITS; m++)
    {
        if (i + 1 >= len)
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (!pulses[i].level || !ir_sony_dur_ok(pulses[i].duration_us, IR_SONY_BIT_MARK))
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (pulses[i + 1].level)
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (ir_sony_dur_ok(pulses[i + 1].duration_us, IR_SONY_BIT_ONE_SPACE))
        {
            bits |= (1u << m);
        }
        else if (!ir_sony_dur_ok(pulses[i + 1].duration_us, IR_SONY_BIT_ZERO_SPACE))
        {
            return ESP_ERR_NOT_FOUND;
        }
        i += 2;
    }

    if (i >= len || !pulses[i].level ||
        !ir_sony_dur_ok(pulses[i].duration_us, IR_SONY_BIT_MARK))
    {
        return ESP_ERR_NOT_FOUND;
    }

    decoded->valid = true;
    decoded->protocol = ROBOT_IR_PROTOCOL_SONY;
    decoded->address = bits & 0x7F;
    decoded->command = (bits >> IR_SONY_ADDR_BITS) & 0x1F;
    return ESP_OK;
}

const ir_codec_t ir_sony_codec = {
    .profile_id = IR_SONY_PROFILE_ID,
    .name = "Sony",
    .carrier_hz = IR_SONY_CARRIER_HZ,
    .encode = ir_sony_encode,
    .decode = ir_sony_decode,
};

static robot_driver_t s_drv;

const robot_driver_t *ir_sony_get_driver(void)
{
    if (s_drv.profile_id[0] == '\0')
    {
        memset(&s_drv, 0, sizeof(s_drv));
        strncpy(s_drv.profile_id, IR_SONY_PROFILE_ID, sizeof(s_drv.profile_id) - 1);
        s_drv.category = ROBOT_CATEGORY_IR_ACTUATOR;
        s_drv.protocol = ROBOT_PROTOCOL_IR;
        s_drv.capabilities = (1u << ROBOT_ACTION_SEND_IR_COMMAND)
                             | (1u << ROBOT_ACTION_LEARN_IR_CODE);
        s_drv.execute = ir_rmt_codec_execute;
        s_drv.probe = ir_rmt_probe;
        s_drv.priv = (void *)&ir_sony_codec;
    }
    return &s_drv;
}
