/**
 * @file ir_rmt.c
 * @brief IR RMT engine, learned-code store and shared driver logic (Phase 5).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ir_rmt.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs_setup.h"
#include "robot_hal.h"

#define TAG "IR_RMT"

#if !defined(CONFIG_ROBOT_IR_TX_GPIO)
#define CONFIG_ROBOT_IR_TX_GPIO 39
#endif
#if !defined(CONFIG_ROBOT_IR_RX_GPIO)
#define CONFIG_ROBOT_IR_RX_GPIO 38
#endif
#if !defined(CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS)
#define CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS 2500
#endif

#define IR_RMT_RESOLUTION_HZ     1000000u  /* 1 us per tick */
#define IR_RMT_MEM_SYMBOLS       192u
#define IR_RMT_TX_TASK_STACK     3072u
#define IR_RMT_TX_TASK_PRIO      5
#define IR_RMT_TX_TASK_CORE      1
#define IR_RMT_RX_FILTER_NS      200000u   /* ignore < 200 us pulses  */
#define IR_RMT_RX_IDLE_NS        50000000u /* 50 ms idle -> frame end */

/* Namespace dedicado a codigos IR aprendidos (separado de `robot_registry`
 * del HAL: el borrado de uno no debe tocar al otro). Hasta la v1 los blobs
 * lr_* vivieron dentro de `robot_registry`; ir_learn_nvs_migrate_legacy()
 * los mueve a `ir_codes` en el primer arranque del motor. */
#define IR_LEARN_NVS_NAMESPACE       "ir_codes"
#define IR_LEARN_NVS_NAMESPACE_LEGACY "robot_registry"
#define IR_LEARN_KEY_PREFIX      "lr_"
#define IR_LEARN_MAGIC           0x494C4D52u /* "ILMR" */
#define IR_LEARN_VERSION         1u
#define IR_LEARN_QUEUE_LEN       4
#define IR_LEARN_WORKER_STACK    4096
#define IR_LEARN_WORKER_PRIO     5
#define IR_LEARN_WORKER_CORE     0
#define IR_LEARN_CACHE_MAX       8

#define IR_CODEC_CARRIER_FALLBACK 38000u

extern const ir_codec_t ir_nec_codec;
extern const ir_codec_t ir_sony_codec;
extern const ir_codec_t ir_rc5_codec;

static const ir_codec_t *const s_codecs[] = {
    &ir_nec_codec,
    &ir_sony_codec,
    &ir_rc5_codec,
};

/* ── RMT engine state ────────────────────────────────────────────────── */

/* El motor IR se arranca BAJO DEMANDA (primer uso real: enviar/capturar/
 * aprender). La BOX-3 base no tiene hardware IR, asi que aplazar la
 * creacion de canales RMT (GDMA + descriptores internos), semaforos,
 * cola y worker NVS libera ~7 KB de SRAM interna / DMA durante todo el
 * arranque y la ignicion de WebRTC. Un dock externo con emisor/receptor
 * activa el motor automaticamente en su primer comando IR. */
typedef enum
{
    IR_ENGINE_OFF = 0,
    IR_ENGINE_READY,
    IR_ENGINE_FAILED,
} ir_engine_state_t;

static ir_engine_state_t s_engine_state = IR_ENGINE_OFF;
static SemaphoreHandle_t s_engine_lock = NULL;

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_tx_encoder = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;
static rmt_channel_handle_t s_rx_chan = NULL;
static SemaphoreHandle_t s_rx_done = NULL;
static SemaphoreHandle_t s_capture_lock = NULL;
static rmt_symbol_word_t *s_rx_buf = NULL; /* PSRAM, 192 symbols */

/* ── Learned-code store state ────────────────────────────────────────── */

typedef struct
{
    uint32_t magic;
    uint8_t  version;
    uint8_t  protocol;
    uint8_t  reserved[2];
    uint32_t id;
    uint32_t address;
    uint32_t command;
    uint16_t pulse_count;
    ir_pulse_t pulses[IR_LEARN_MAX_PULSES];
    uint32_t crc32;
} ir_learn_blob_t;

typedef struct
{
    uint32_t id;
    ir_decoded_t decoded;
    uint16_t pulse_count;
    ir_pulse_t *pulses; /* PSRAM copy */
} ir_learn_entry_t;

static ir_learn_entry_t s_learn_cache[IR_LEARN_CACHE_MAX];
static SemaphoreHandle_t s_learn_mutex = NULL;
static QueueHandle_t s_learn_queue = NULL;

static uint32_t ir_learn_blob_crc(const ir_learn_blob_t *b)
{
    return esp_rom_crc32_le(0, (const uint8_t *)b,
                            offsetof(ir_learn_blob_t, crc32));
}

static ir_learn_entry_t *ir_learn_find(uint32_t id)
{
    for (size_t i = 0; i < IR_LEARN_CACHE_MAX; i++)
    {
        if (s_learn_cache[i].pulses != NULL && s_learn_cache[i].id == id)
        {
            return &s_learn_cache[i];
        }
    }
    return NULL;
}

static ir_learn_entry_t *ir_learn_free_slot(void)
{
    ir_learn_entry_t *oldest = &s_learn_cache[0];
    for (size_t i = 0; i < IR_LEARN_CACHE_MAX; i++)
    {
        if (s_learn_cache[i].pulses == NULL)
        {
            return &s_learn_cache[i];
        }
        if (s_learn_cache[i].id < oldest->id)
        {
            oldest = &s_learn_cache[i]; /* eviction arbitraria: id mas bajo */
        }
    }
    return oldest;
}

/* ── Pulse helpers ───────────────────────────────────────────────────── */

static uint16_t ir_pack_symbols(const ir_pulse_t *pulses, uint16_t len,
                                rmt_symbol_word_t *sym, uint16_t max_sym)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i + 1 <= len; i += 2)
    {
        if (n >= max_sym)
        {
            break;
        }
        rmt_symbol_word_t s = {0};
        s.duration0 = (uint16_t)((pulses[i].duration_us > 0xFFFF) ? 0xFFFF : pulses[i].duration_us);
        s.level0 = (uint32_t)pulses[i].level;
        if (i + 1 < len)
        {
            s.duration1 = (uint16_t)((pulses[i + 1].duration_us > 0xFFFF) ? 0xFFFF : pulses[i + 1].duration_us);
            s.level1 = (uint32_t)pulses[i + 1].level;
        }
        sym[n++] = s;
    }
    return n;
}

/* ── Engine lifecycle: lazy boot on first use ────────────────────────── */

static esp_err_t ir_rmt_engine_boot(void);

static esp_err_t ir_rmt_engine_ensure(void)
{
    if (s_engine_state == IR_ENGINE_READY)
    {
        return ESP_OK;
    }
    if (s_engine_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* ir_rmt_init() no llamado */
    }

    /* Todos los puntos de entrada corren en contexto de tarea (tool task /
     * exec de driver), nunca en ISR: el lock serializa un arranque
     * concurrente desde dos tareas sin riesgo de condiciones de carrera. */
    if (xSemaphoreTake(s_engine_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_engine_state != IR_ENGINE_READY)
    {
        const esp_err_t err = ir_rmt_engine_boot();
        if (err == ESP_OK)
        {
            s_engine_state = IR_ENGINE_READY;
        }
        else
        {
            s_engine_state = IR_ENGINE_FAILED;
            ESP_LOGE(TAG, "Arranque bajo demanda del motor IR fallo: %s",
                     esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_engine_lock);

    return (s_engine_state == IR_ENGINE_READY) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

/* ── TX: non-blocking transmit task ──────────────────────────────────── */

typedef struct
{
    rmt_symbol_word_t *symbols; /* PSRAM copy */
    uint16_t count;
    uint32_t carrier_hz;
} ir_tx_param_t;

static void ir_rmt_tx_task(void *arg)
{
    ir_tx_param_t *p = (ir_tx_param_t *)arg;
    if (p != NULL)
    {
        if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            const rmt_carrier_config_t carrier = {
                .frequency_hz = p->carrier_hz,
                .flags.polarity_active_low = 0,
            };
            rmt_apply_carrier(s_tx_chan, &carrier);

            const rmt_transmit_config_t tx_cfg = {
                .loop_count = 0,
                .flags.eot_level = 0,
            };
            const esp_err_t err = rmt_transmit(s_tx_chan, s_tx_encoder,
                                               p->symbols,
                                               p->count * sizeof(rmt_symbol_word_t),
                                               &tx_cfg);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "rmt_transmit fallo: %s", esp_err_to_name(err));
            }
            else
            {
                rmt_tx_wait_all_done(s_tx_chan, 500);
            }
            xSemaphoreGive(s_tx_mutex);
        }
        free(p->symbols);
        free(p);
    }
    vTaskDelete(NULL);
}

esp_err_t ir_rmt_send_pulses(const ir_pulse_t *pulses, uint16_t len,
                             uint32_t carrier_hz)
{
    const esp_err_t eng = ir_rmt_engine_ensure();
    if (eng != ESP_OK)
    {
        return eng;
    }
    if (pulses == NULL || len == 0 || s_tx_chan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t sym_count = (len + 1) / 2;
    rmt_symbol_word_t *symbols = heap_caps_malloc(
        sym_count * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (symbols == NULL)
    {
        symbols = malloc(sym_count * sizeof(rmt_symbol_word_t)); /* fallback DRAM */
    }
    if (symbols == NULL)
    {
        ESP_LOGE(TAG, "send: sin memoria para simbolos");
        return ESP_ERR_NO_MEM;
    }

    const uint16_t packed = ir_pack_symbols(pulses, len, symbols, sym_count);
    if (packed == 0)
    {
        free(symbols);
        return ESP_ERR_INVALID_ARG;
    }

    ir_tx_param_t *param = heap_caps_malloc(sizeof(ir_tx_param_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (param == NULL)
    {
        param = malloc(sizeof(ir_tx_param_t)); /* fallback DRAM */
    }
    if (param == NULL)
    {
        free(symbols);
        ESP_LOGE(TAG, "send: sin memoria para parametros");
        return ESP_ERR_NO_MEM;
    }

    param->symbols = symbols;
    param->count = packed;
    param->carrier_hz = carrier_hz ? carrier_hz : IR_CODEC_CARRIER_FALLBACK;

    if (xTaskCreatePinnedToCoreWithCaps(ir_rmt_tx_task, "ir_tx",
                                        IR_RMT_TX_TASK_STACK, param,
                                        IR_RMT_TX_TASK_PRIO, NULL,
                                        IR_RMT_TX_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        ESP_LOGE(TAG, "send: no se pudo crear la tarea de TX");
        free(symbols);
        free(param);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TX IR encolado: %u pulsos, portadora %lu Hz", (unsigned)len,
             (unsigned long)param->carrier_hz);
    return ESP_OK;
}

/* ── RX: learning capture ────────────────────────────────────────────── */

static bool ir_rmt_rx_done_cb(rmt_channel_handle_t chan,
                              const rmt_rx_done_event_data_t *edata,
                              void *ctx)
{
    (void)chan;
    (void)edata;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &hp);
    return hp == pdTRUE;
}

static void ir_rmt_trim_pulses(ir_pulse_t *pulses, uint16_t *len)
{
    /* Normalizar polaridad del receptor: los TSOP38 son activos-bajo
     * (marca = nivel bajo). Toda trama válida empieza con marca, así que
     * si el primer pulso es un espacio, invertimos todos los niveles. */
    if (*len > 0 && !pulses[0].level)
    {
        for (uint16_t i = 0; i < *len; i++)
        {
            pulses[i].level = !pulses[i].level;
        }
    }

    /* Quitar espacios iniciales (idle previo, cualquier duración). */
    uint16_t start = 0;
    while (start < *len && !pulses[start].level)
    {
        start++;
    }

    /* Quitar el gap de idle final (espacio que disparo el fin de trama).
     * Un espacio corto al final puede ser la mitad B de RC5 (889 us). */
    uint16_t end = *len;
    while (end > start && !pulses[end - 1].level && pulses[end - 1].duration_us > 20000)
    {
        end--;
    }

    if (start > 0)
    {
        memmove(pulses, &pulses[start], (end - start) * sizeof(ir_pulse_t));
    }
    *len = (uint16_t)(end - start);
}

esp_err_t ir_rmt_capture_pulses(ir_pulse_t *pulses, uint16_t max_pulses,
                                uint16_t *out_len, uint32_t timeout_ms)
{
    const esp_err_t eng = ir_rmt_engine_ensure();
    if (eng != ESP_OK)
    {
        return eng;
    }
    if (pulses == NULL || out_len == NULL || s_rx_chan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    if (xSemaphoreTake(s_capture_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    /* Descartar cualquier captura previa sin consumir. */
    xSemaphoreTake(s_rx_done, 0);

    esp_err_t err = rmt_enable(s_rx_chan);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_capture_lock);
        return err;
    }

    const rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = IR_RMT_RX_FILTER_NS,
        .signal_range_max_ns = IR_RMT_RX_IDLE_NS,
    };
    err = rmt_receive(s_rx_chan, s_rx_buf, IR_RMT_MEM_SYMBOLS * sizeof(rmt_symbol_word_t),
                      &rx_cfg);
    if (err != ESP_OK)
    {
        rmt_disable(s_rx_chan);
        xSemaphoreGive(s_capture_lock);
        return err;
    }

    if (xSemaphoreTake(s_rx_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Captura IR agotada (%lu ms), no se recibio senal",
                 (unsigned long)timeout_ms);
        rmt_disable(s_rx_chan);
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_TIMEOUT;
    }

    rmt_disable(s_rx_chan);
    xSemaphoreGive(s_capture_lock);

    uint16_t n = 0;
    for (size_t i = 0; i < IR_RMT_MEM_SYMBOLS && n < max_pulses; i++)
    {
        if (s_rx_buf[i].duration0 > 0)
        {
            pulses[n].duration_us = s_rx_buf[i].duration0;
            pulses[n].level = (s_rx_buf[i].level0 != 0);
            n++;
        }
        if (s_rx_buf[i].duration1 > 0 && n < max_pulses)
        {
            pulses[n].duration_us = s_rx_buf[i].duration1;
            pulses[n].level = (s_rx_buf[i].level1 != 0);
            n++;
        }
    }

    ir_rmt_trim_pulses(pulses, &n);

    if (n < 3)
    {
        ESP_LOGW(TAG, "Captura IR demasiado corta (%u pulsos)", (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }

    *out_len = n;
    ESP_LOGI(TAG, "Captura IR: %u pulsos", (unsigned)n);
    return ESP_OK;
}

/* ── Probe: virtual presence ─────────────────────────────────────────── */

esp_err_t ir_rmt_probe(robot_driver_t *drv, const robot_endpoint_t *ep,
                       bool *present)
{
    (void)drv;
    (void)ep;
    if (present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Los emisores IR no tienen enlace físico: presencia virtual. */
    *present = true;
    return ESP_OK;
}

/* ── Shared driver execute ───────────────────────────────────────────── */

static robot_result_code_t ir_map_error(esp_err_t err)
{
    switch (err)
    {
    case ESP_ERR_TIMEOUT:        return ROBOT_RESULT_ERR_TIMEOUT;
    case ESP_ERR_NOT_FOUND:      return ROBOT_RESULT_ERR_NOT_FOUND;
    case ESP_ERR_INVALID_ARG:    return ROBOT_RESULT_ERR_INVALID_ARG;
    case ESP_ERR_NOT_SUPPORTED:  return ROBOT_RESULT_ERR_UNSUPPORTED;
    case ESP_ERR_NO_MEM:         return ROBOT_RESULT_ERR_TRANSPORT;
    default:                     return ROBOT_RESULT_ERR_TRANSPORT;
    }
}

static uint32_t ir_carrier_for(const ir_codec_t *codec, const ir_decoded_t *decoded)
{
    if (decoded != NULL && decoded->valid && decoded->protocol == ROBOT_IR_PROTOCOL_SONY)
    {
        return 40000u;
    }
    return codec->carrier_hz;
}

static esp_err_t ir_execute_send(const ir_codec_t *codec,
                                 const robot_device_t *dev,
                                 const robot_action_params_t *params,
                                 robot_result_t *out)
{
    ir_pulse_t pulses[IR_LEARN_MAX_PULSES];
    uint16_t len = 0;
    uint32_t carrier = codec->carrier_hz;
    const char *what = NULL;

    if (params != NULL && params->raw_timings != NULL && params->raw_len > 0)
    {
        /* Modo RAW: tren de duraciones alternando mark/space (mark primero). */
        len = (params->raw_len > IR_LEARN_MAX_PULSES) ? IR_LEARN_MAX_PULSES : params->raw_len;
        for (uint16_t i = 0; i < len; i++)
        {
            pulses[i].duration_us = params->raw_timings[i];
            pulses[i].level = (i % 2) == 0;
        }
        what = "RAW";
    }
    else if (params != NULL && (params->ir_address != 0 || params->ir_command != 0))
    {
        /* Codificar con el codec del driver. */
        const esp_err_t err = codec->encode(params->ir_address, params->ir_command,
                                            pulses, IR_LEARN_MAX_PULSES, &len);
        if (err != ESP_OK)
        {
            out->code = ir_map_error(err);
            snprintf(out->detail, sizeof(out->detail),
                     "Fallo codificando IR %s: %s", codec->name, esp_err_to_name(err));
            return err;
        }
        what = codec->name;
    }
    else
    {
        /* Rejugar el último código aprendido para este dispositivo. */
        const ir_pulse_t *learned = NULL;
        uint16_t llen = 0;
        const ir_decoded_t *decoded = ir_learn_get(dev->id, &learned, &llen);
        if (decoded == NULL || learned == NULL || llen == 0)
        {
            out->code = ROBOT_RESULT_ERR_INVALID_ARG;
            snprintf(out->detail, sizeof(out->detail),
                     "IR: sin direccion/comando y sin codigo aprendido para '%s'",
                     dev->alias);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(pulses, learned, llen * sizeof(ir_pulse_t));
        len = llen;
        carrier = ir_carrier_for(codec, decoded);
        what = "aprendido";
    }

    const esp_err_t err = ir_rmt_send_pulses(pulses, len, carrier);
    if (err != ESP_OK)
    {
        out->code = ir_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "Fallo la transmision IR a '%s': %s", dev->alias, esp_err_to_name(err));
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    snprintf(out->detail, sizeof(out->detail),
             "IR %s enviado a '%s'", what, dev->alias);
    return ESP_OK;
}

static esp_err_t ir_execute_learn(const ir_codec_t *codec,
                                  const robot_device_t *dev,
                                  robot_result_t *out)
{
    (void)codec;
    ir_pulse_t pulses[IR_LEARN_MAX_PULSES];
    uint16_t len = 0;

    const esp_err_t err = ir_rmt_capture_pulses(pulses, IR_LEARN_MAX_PULSES, &len,
                                                CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        out->code = ir_map_error(err);
        snprintf(out->detail, sizeof(out->detail),
                 "No se recibio senal IR en %lu ms: apunta el mando y pulsa el boton",
                 (unsigned long)CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS);
        return err;
    }

    /* Orden de decodificacion: codec del dispositivo primero, luego el resto. */
    const ir_codec_t *order[4];
    size_t n_order = 0;
    order[n_order++] = codec;
    for (size_t i = 0; i < sizeof(s_codecs) / sizeof(s_codecs[0]) && n_order < 4; i++)
    {
        if (s_codecs[i] != codec)
        {
            order[n_order++] = s_codecs[i];
        }
    }

    ir_decoded_t best = {0};
    const ir_codec_t *match = NULL;
    for (size_t i = 0; i < n_order; i++)
    {
        ir_decoded_t d = {0};
        if (order[i]->decode(pulses, len, &d) == ESP_OK && d.valid)
        {
            best = d;
            match = order[i];
            break;
        }
    }

    /* Guardar siempre la captura (RAW) + decodificacion si hubo match. */
    ir_decoded_t stored = {0};
    if (match != NULL)
    {
        stored = best;
    }
    ir_learn_save(dev->id, pulses, len, &stored);

    if (match != NULL)
    {
        out->code = ROBOT_RESULT_OK;
        snprintf(out->detail, sizeof(out->detail),
                 "Codigo IR %s aprendido para '%s' (addr=0x%lX, cmd=0x%lX)",
                 match->name, dev->alias, (unsigned long)best.address,
                 (unsigned long)best.command);
    }
    else
    {
        out->code = ROBOT_RESULT_OK;
        snprintf(out->detail, sizeof(out->detail),
                 "Senal IR capturada para '%s' (%u pulsos): protocolo no reconocido, guardada como RAW",
                 dev->alias, (unsigned)len);
    }
    return ESP_OK;
}

esp_err_t ir_rmt_codec_execute(robot_driver_t *drv, const char *alias,
                               robot_action_id_t action,
                               const robot_action_params_t *params,
                               robot_result_t *out)
{
    if (drv == NULL || alias == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->code = ROBOT_RESULT_ERR_TRANSPORT;

    const ir_codec_t *codec = (const ir_codec_t *)drv->priv;
    if (codec == NULL)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail), "Driver IR sin codec");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (action != ROBOT_ACTION_SEND_IR_COMMAND && action != ROBOT_ACTION_LEARN_IR_CODE)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), codec->profile_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        out->code = ROBOT_RESULT_ERR_NOT_FOUND;
        snprintf(out->detail, sizeof(out->detail), "Dispositivo '%s' no registrado", alias);
        return ESP_ERR_NOT_FOUND;
    }

    if (action == ROBOT_ACTION_SEND_IR_COMMAND)
    {
        return ir_execute_send(codec, dev, params, out);
    }
    return ir_execute_learn(codec, dev, out);
}

/* ── Learned-code store (RAM cache + NVS worker) ─────────────────────── */

typedef enum
{
    IR_LEARN_OP_SAVE = 1,
    IR_LEARN_OP_LOAD_ALL,
    IR_LEARN_OP_DELETE,
    IR_LEARN_OP_DELETE_ALL,
    IR_LEARN_OP_MIGRATE,
} ir_learn_op_t;

typedef struct
{
    ir_learn_op_t op;
    ir_learn_blob_t blob;
} ir_learn_queue_item_t;

static void ir_learn_cache_store_locked(const ir_learn_blob_t *blob)
{
    ir_learn_entry_t *slot = ir_learn_find(blob->id);
    if (slot == NULL)
    {
        slot = ir_learn_free_slot();
    }
    if (slot->pulses != NULL)
    {
        free(slot->pulses);
    }
    slot->id = blob->id;
    slot->decoded.valid = (blob->protocol != ROBOT_IR_PROTOCOL_RAW);
    slot->decoded.protocol = (robot_ir_protocol_t)blob->protocol;
    slot->decoded.address = blob->address;
    slot->decoded.command = blob->command;
    slot->pulse_count = blob->pulse_count;
    slot->pulses = heap_caps_malloc(blob->pulse_count * sizeof(ir_pulse_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (slot->pulses == NULL)
    {
        slot->pulses = malloc(blob->pulse_count * sizeof(ir_pulse_t));
    }
    if (slot->pulses != NULL)
    {
        memcpy(slot->pulses, blob->pulses, blob->pulse_count * sizeof(ir_pulse_t));
        slot->pulse_count = blob->pulse_count;
    }
    else
    {
        slot->pulse_count = 0;
        slot->decoded.valid = false;
    }
}

static esp_err_t ir_learn_nvs_save(const ir_learn_blob_t *blob)
{
    nvs_setup_mutex_init();
    nvs_lock();

    esp_err_t err = ESP_OK;
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(IR_LEARN_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (open_err == ESP_OK)
    {
        char key[16];
        snprintf(key, sizeof(key), IR_LEARN_KEY_PREFIX "%08lx", (unsigned long)blob->id);
        err = nvs_set_blob(handle, key, blob, sizeof(*blob));
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    else
    {
        err = open_err;
    }
    nvs_unlock();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Codigo IR aprendido persistido (id=%08lx)", (unsigned long)blob->id);
    }
    else
    {
        ESP_LOGE(TAG, "Fallo persistiendo codigo IR: %s", esp_err_to_name(err));
    }
    return err;
}

static void ir_learn_nvs_load_all(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(IR_LEARN_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_err != ESP_OK)
    {
        nvs_unlock();
        return;
    }

    int loaded = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NULL, IR_LEARN_NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (res == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, IR_LEARN_KEY_PREFIX, strlen(IR_LEARN_KEY_PREFIX)) == 0)
        {
            /* Static: el worker es una unica tarea; un local de 2 KB
             * agotaria su stack de 4096 B durante el load-all. */
            static ir_learn_blob_t blob;
            memset(&blob, 0, sizeof(blob));
            size_t len = sizeof(blob);
            const esp_err_t err = nvs_get_blob(handle, info.key, &blob, &len);
            if (err == ESP_OK && len == sizeof(blob) &&
                blob.magic == IR_LEARN_MAGIC && blob.version == IR_LEARN_VERSION &&
                blob.pulse_count <= IR_LEARN_MAX_PULSES &&
                blob.crc32 == ir_learn_blob_crc(&blob))
            {
                if (xSemaphoreTake(s_learn_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    ir_learn_cache_store_locked(&blob);
                    xSemaphoreGive(s_learn_mutex);
                }
                loaded++;
            }
            else
            {
                ESP_LOGW(TAG, "Blob IR %s corrupto, omitido", info.key);
            }
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);
    nvs_unlock();

    ESP_LOGI(TAG, "Codigos IR aprendidos: %d cargados de NVS", loaded);
}

/* Migracion one-shot: los blobs lr_* historicamente viven en el namespace
 * `robot_registry` (compartido con el HAL registry). Se reescriben en
 * `ir_codes` y se borran del legado — idempotente y sin perdida: si el
 * destino ya tiene la clave, el origen se limpia igualmente. */
static void ir_learn_nvs_migrate_legacy(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t legacy;
    if (nvs_open(IR_LEARN_NVS_NAMESPACE_LEGACY, NVS_READONLY, &legacy) != ESP_OK)
    {
        nvs_unlock();
        return;
    }

    nvs_handle_t target;
    if (nvs_open(IR_LEARN_NVS_NAMESPACE, NVS_READWRITE, &target) != ESP_OK)
    {
        nvs_close(legacy);
        nvs_unlock();
        ESP_LOGW(TAG, "Migracion IR: no se pudo abrir '%s'", IR_LEARN_NVS_NAMESPACE);
        return;
    }

    int migrated = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NULL, IR_LEARN_NVS_NAMESPACE_LEGACY,
                                   NVS_TYPE_BLOB, &it);
    while (res == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, IR_LEARN_KEY_PREFIX, strlen(IR_LEARN_KEY_PREFIX)) == 0)
        {
            /* Static: el worker es una unica tarea; un local de 2 KB
             * agotaria su stack de 4096 B. */
            static ir_learn_blob_t blob;
            memset(&blob, 0, sizeof(blob));
            size_t len = sizeof(blob);
            if (nvs_get_blob(legacy, info.key, &blob, &len) == ESP_OK &&
                len == sizeof(blob) && blob.magic == IR_LEARN_MAGIC &&
                blob.version == IR_LEARN_VERSION &&
                blob.pulse_count <= IR_LEARN_MAX_PULSES &&
                blob.crc32 == ir_learn_blob_crc(&blob))
            {
                char new_key[16];
                snprintf(new_key, sizeof(new_key), IR_LEARN_KEY_PREFIX "%08lx",
                         (unsigned long)blob.id);
                size_t have = 0;
                const esp_err_t gerr = nvs_get_blob(target, new_key, NULL, &have);
                if (gerr == ESP_ERR_NVS_NOT_FOUND)
                {
                    if (nvs_set_blob(target, new_key, &blob, sizeof(blob)) == ESP_OK)
                    {
                        migrated++;
                    }
                }
                else if (gerr != ESP_OK)
                {
                    ESP_LOGW(TAG, "Migracion IR: fallo verificando %s", new_key);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Migracion IR: blob legado %s corrupto, omitido", info.key);
            }
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    if (migrated > 0 && nvs_commit(target) != ESP_OK)
    {
        ESP_LOGW(TAG, "Migracion IR: commit de destino fallo");
    }
    nvs_close(target);

    /* Limpieza del legado: los blobs validos ya viven en `ir_codes` y los
     * corruptos no tienen valor — el namespace queda libre para el HAL. */
    nvs_close(legacy);
    if (nvs_open(IR_LEARN_NVS_NAMESPACE_LEGACY, NVS_READWRITE, &legacy) == ESP_OK)
    {
        int erased = 0;
        nvs_iterator_t rit = NULL;
        res = nvs_entry_find(NULL, IR_LEARN_NVS_NAMESPACE_LEGACY, NVS_TYPE_BLOB, &rit);
        while (res == ESP_OK)
        {
            nvs_entry_info_t info;
            nvs_entry_info(rit, &info);
            if (strncmp(info.key, IR_LEARN_KEY_PREFIX, strlen(IR_LEARN_KEY_PREFIX)) == 0 &&
                nvs_erase_key(legacy, info.key) == ESP_OK)
            {
                erased++;
            }
            res = nvs_entry_next(&rit);
        }
        nvs_release_iterator(rit);
        if (erased > 0)
        {
            nvs_commit(legacy);
        }
        nvs_close(legacy);
    }

    nvs_unlock();

    if (migrated > 0)
    {
        ESP_LOGI(TAG, "Migracion IR: %d codigo(s) movidos de '%s' a '%s'",
                 migrated, IR_LEARN_NVS_NAMESPACE_LEGACY, IR_LEARN_NVS_NAMESPACE);
    }
}

static void ir_learn_nvs_delete(uint32_t id)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t handle;
    if (nvs_open(IR_LEARN_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
    {
        char key[16];
        snprintf(key, sizeof(key), IR_LEARN_KEY_PREFIX "%08lx", (unsigned long)id);
        const esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
        {
            nvs_commit(handle);
            ESP_LOGI(TAG, "Codigo IR id=%08lx eliminado de NVS", (unsigned long)id);
        }
        else
        {
            ESP_LOGW(TAG, "Fallo borrando codigo IR %s: %s", key, esp_err_to_name(err));
        }
        nvs_close(handle);
    }
    nvs_unlock();
}

static void ir_learn_nvs_delete_all(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t handle;
    if (nvs_open(IR_LEARN_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
    {
        int erased = 0;
        nvs_iterator_t it = NULL;
        esp_err_t res = nvs_entry_find(NULL, IR_LEARN_NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
        while (res == ESP_OK)
        {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (strncmp(info.key, IR_LEARN_KEY_PREFIX, strlen(IR_LEARN_KEY_PREFIX)) == 0 &&
                nvs_erase_key(handle, info.key) == ESP_OK)
            {
                erased++;
            }
            res = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        if (erased > 0)
        {
            nvs_commit(handle);
        }
        nvs_close(handle);
        ESP_LOGI(TAG, "%d codigo(s) IR eliminados de NVS", erased);
    }
    nvs_unlock();
}

static void ir_learn_worker_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        ir_learn_queue_item_t *item = NULL;
        if (xQueueReceive(s_learn_queue, &item, portMAX_DELAY) != pdTRUE || item == NULL)
        {
            continue;
        }
        if (item->op == IR_LEARN_OP_SAVE)
        {
            ir_learn_nvs_save(&item->blob);
        }
        else if (item->op == IR_LEARN_OP_LOAD_ALL)
        {
            ir_learn_nvs_load_all();
        }
        else if (item->op == IR_LEARN_OP_DELETE)
        {
            ir_learn_nvs_delete(item->blob.id);
        }
        else if (item->op == IR_LEARN_OP_DELETE_ALL)
        {
            ir_learn_nvs_delete_all();
        }
        else if (item->op == IR_LEARN_OP_MIGRATE)
        {
            ir_learn_nvs_migrate_legacy();
        }
        heap_caps_free(item);
    }
}

/* ¿Puede `b` (nuevo) colapsar sobre `a` (ya en cola)? SAVE/DELETE solo
 * sobre el mismo id; las demas (LOAD_ALL / DELETE_ALL / MIGRATE) colapsan
 * consigo mismas (ejecutarlas dos veces es redundante). */
static bool ir_learn_collapsible(const ir_learn_queue_item_t *a,
                                 const ir_learn_queue_item_t *b)
{
    if (a->op != b->op)
    {
        return false;
    }
    if (a->op == IR_LEARN_OP_SAVE || a->op == IR_LEARN_OP_DELETE)
    {
        return a->blob.id == b->blob.id;
    }
    return true;
}

static esp_err_t ir_learn_enqueue(const ir_learn_queue_item_t *item)
{
    if (s_learn_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ir_learn_queue_item_t *copy = heap_caps_malloc(sizeof(ir_learn_queue_item_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para cola de aprendizaje");
        return ESP_ERR_NO_MEM;
    }
    *copy = *item;
    if (xQueueSend(s_learn_queue, &copy, 0) != pdTRUE)
    {
        /* Cola llena → coalescing: reemplazar la operacion pendiente del
         * mismo tipo+id (el ultimo estado gana) en lugar de descartar. */
        ir_learn_queue_item_t *replacement = NULL;
        for (int i = 0; i < IR_LEARN_QUEUE_LEN && replacement == NULL; i++)
        {
            ir_learn_queue_item_t *queued = NULL;
            if (xQueueReceive(s_learn_queue, &queued, 0) != pdTRUE)
            {
                break;
            }
            if (queued != NULL)
            {
                if (ir_learn_collapsible(queued, item))
                {
                    *queued = *item;
                    replacement = queued;
                }
                else if (xQueueSend(s_learn_queue, &queued, 0) != pdTRUE)
                {
                    heap_caps_free(queued); /* inalcanzable: slot recien vaciado */
                }
            }
        }
        heap_caps_free(copy);
        if (replacement == NULL)
        {
            ESP_LOGW(TAG, "Cola de aprendizaje llena, persistencia omitida");
            return ESP_ERR_TIMEOUT;
        }
        if (xQueueSend(s_learn_queue, &replacement, 0) != pdTRUE)
        {
            heap_caps_free(replacement);
            ESP_LOGW(TAG, "Cola de aprendizaje llena, persistencia omitida");
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGD(TAG, "Operacion IR colapsada con una pendiente equivalente (op=%d, id=%08lx)",
                 (int)item->op, (unsigned long)item->blob.id);
    }
    return ESP_OK;
}

esp_err_t ir_learn_save(uint32_t id, const ir_pulse_t *pulses, uint16_t len,
                        const ir_decoded_t *decoded)
{
    const esp_err_t eng = ir_rmt_engine_ensure();
    if (eng != ESP_OK)
    {
        return eng;
    }
    if (pulses == NULL || len == 0 || len > IR_LEARN_MAX_PULSES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Caché RAM primero (lectura inmediata para el replay). */
    if (s_learn_mutex != NULL)
    {
        ir_learn_blob_t blob = {0};
        blob.magic = IR_LEARN_MAGIC;
        blob.version = IR_LEARN_VERSION;
        blob.protocol = (decoded != NULL) ? (uint8_t)decoded->protocol : (uint8_t)ROBOT_IR_PROTOCOL_RAW;
        blob.id = id;
        blob.address = (decoded != NULL) ? decoded->address : 0;
        blob.command = (decoded != NULL) ? decoded->command : 0;
        blob.pulse_count = len;
        memcpy(blob.pulses, pulses, len * sizeof(ir_pulse_t));
        blob.crc32 = ir_learn_blob_crc(&blob);

        if (xSemaphoreTake(s_learn_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            ir_learn_cache_store_locked(&blob);
            xSemaphoreGive(s_learn_mutex);
        }

        const ir_learn_queue_item_t item = { .op = IR_LEARN_OP_SAVE, .blob = blob };
        return ir_learn_enqueue(&item);
    }
    return ESP_ERR_INVALID_STATE;
}

const ir_decoded_t *ir_learn_get(uint32_t id, const ir_pulse_t **pulses,
                                 uint16_t *len)
{
    if (ir_rmt_engine_ensure() != ESP_OK || s_learn_mutex == NULL ||
        pulses == NULL || len == NULL)
    {
        return NULL;
    }

    if (xSemaphoreTake(s_learn_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return NULL;
    }
    const ir_learn_entry_t *entry = ir_learn_find(id);
    if (entry != NULL && entry->pulses != NULL)
    {
        *pulses = entry->pulses;
        *len = entry->pulse_count;
        xSemaphoreGive(s_learn_mutex);
        return &entry->decoded;
    }
    xSemaphoreGive(s_learn_mutex);
    return NULL;
}

static void ir_learn_cache_evict_locked(uint32_t id)
{
    ir_learn_entry_t *slot = ir_learn_find(id);
    if (slot != NULL)
    {
        free(slot->pulses);
        slot->pulses = NULL;
        slot->pulse_count = 0;
        slot->decoded.valid = false;
    }
}

static void ir_learn_cache_clear_locked(void)
{
    for (size_t i = 0; i < IR_LEARN_CACHE_MAX; i++)
    {
        if (s_learn_cache[i].pulses != NULL)
        {
            free(s_learn_cache[i].pulses);
            s_learn_cache[i].pulses = NULL;
        }
        s_learn_cache[i].pulse_count = 0;
        s_learn_cache[i].decoded.valid = false;
    }
}

esp_err_t ir_learn_delete(uint32_t id)
{
    if (s_learn_mutex == NULL || s_learn_queue == NULL)
    {
        /* Motor nunca arrancado: nada aprendido ni persistido por aqui. */
        return ESP_OK;
    }
    if (xSemaphoreTake(s_learn_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        ir_learn_cache_evict_locked(id);
        xSemaphoreGive(s_learn_mutex);
    }
    const ir_learn_queue_item_t item = { .op = IR_LEARN_OP_DELETE, .blob = { .id = id } };
    return ir_learn_enqueue(&item);
}

esp_err_t ir_learn_delete_all(void)
{
    if (s_learn_mutex == NULL || s_learn_queue == NULL)
    {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_learn_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        ir_learn_cache_clear_locked();
        xSemaphoreGive(s_learn_mutex);
    }
    const ir_learn_queue_item_t item = { .op = IR_LEARN_OP_DELETE_ALL };
    return ir_learn_enqueue(&item);
}

/* ── Engine init (lazy: se ejecuta en el primer uso real) ────────────── */

static esp_err_t ir_rmt_engine_boot(void)
{
    /* TX: 1 MHz, 192 symbols, carrier configurable por envío. */
    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = CONFIG_ROBOT_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = IR_RMT_MEM_SYMBOLS,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_tx_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_tx_channel fallo: %s", esp_err_to_name(err));
        return err;
    }

    /* Estructura vacia en IDF v5.4: cualquier inicializador sobraria. */
    rmt_copy_encoder_config_t copy_cfg;
    err = rmt_new_copy_encoder(&copy_cfg, &s_tx_encoder);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_copy_encoder fallo: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_tx_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_enable(TX) fallo: %s", esp_err_to_name(err));
        return err;
    }

    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /* RX: 1 MHz, filtro de 200 us (signal_range_min), fin de trama tras
     * 50 ms de nivel constante (signal_range_max = idle). */
    const rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = CONFIG_ROBOT_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = IR_RMT_MEM_SYMBOLS,
    };
    err = rmt_new_rx_channel(&rx_cfg, &s_rx_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_new_rx_channel fallo: %s", esp_err_to_name(err));
        return err;
    }

    s_rx_done = xSemaphoreCreateBinary();
    s_capture_lock = xSemaphoreCreateMutex();
    if (s_rx_done == NULL || s_capture_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_rx_buf = heap_caps_malloc(IR_RMT_MEM_SYMBOLS * sizeof(rmt_symbol_word_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rx_buf == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para buffer RX");
        return ESP_ERR_NO_MEM;
    }

    const rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_rmt_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, s_rx_done);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt_rx_register_event_callbacks fallo: %s", esp_err_to_name(err));
        return err;
    }

    /* Almacén de códigos aprendidos: caché RAM + worker NVS (SRAM interna,
     * core 0, mutex legado) + carga inicial de blobs lr_*. Solo existe
     * cuando el motor se arranca: si no hay hardware IR, nada se reserva. */
    s_learn_mutex = xSemaphoreCreateMutex();
    if (s_learn_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_learn_queue = xQueueCreate(IR_LEARN_QUEUE_LEN, sizeof(ir_learn_queue_item_t *));
    if (s_learn_queue == NULL)
    {
        ESP_LOGE(TAG, "No se pudo crear la cola de aprendizaje IR");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(ir_learn_worker_task,
                                "ir_learn",
                                IR_LEARN_WORKER_STACK,
                                NULL,
                                IR_LEARN_WORKER_PRIO,
                                NULL,
                                IR_LEARN_WORKER_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "No se pudo crear la tarea de aprendizaje IR");
        return ESP_ERR_NO_MEM;
    }

    /* Migracion one-shot encolada antes del LOAD_ALL: los blobs lr_* que
     * aun vivan en `robot_registry` (v1) se mueven a `ir_codes`. */
    ir_learn_queue_item_t *mig = heap_caps_malloc(sizeof(ir_learn_queue_item_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mig != NULL)
    {
        memset(mig, 0, sizeof(*mig));
        mig->op = IR_LEARN_OP_MIGRATE;
        if (xQueueSend(s_learn_queue, &mig, 0) != pdTRUE)
        {
            heap_caps_free(mig);
        }
    }

    /* LOAD_ALL se encola como item HEAP (PSRAM) — nunca como local de
     * pila: ir_learn_blob_t pesa ~2 KB y el stack de la tarea main es de
     * solo 3584 B (un local aqui desbordaba la pila al arrancar). */
    ir_learn_queue_item_t *load = heap_caps_malloc(sizeof(ir_learn_queue_item_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (load != NULL)
    {
        memset(load, 0, sizeof(*load));
        load->op = IR_LEARN_OP_LOAD_ALL;
        if (xQueueSend(s_learn_queue, &load, 0) != pdTRUE)
        {
            heap_caps_free(load);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para la carga inicial de codigos IR");
    }

    ESP_LOGI(TAG, "Motor IR RMT activo: TX=GPIO%d (38/40 kHz), RX=GPIO%d, learn %lu ms",
             CONFIG_ROBOT_IR_TX_GPIO, CONFIG_ROBOT_IR_RX_GPIO,
             (unsigned long)CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t ir_rmt_init(void)
{
    /* Arranque ligero: solo crea el lock de inicialización. Los canales
     * RMT, GDMA, semaforos, cola y worker NVS quedan aplazados hasta el
     * primer uso real (ir_rmt_engine_ensure) — la BOX-3 sin dock IR no
     * consume SRAM interna ni DMA por el subsistema IR. */
    if (s_engine_state != IR_ENGINE_OFF || s_engine_lock != NULL)
    {
        return ESP_OK;
    }
    s_engine_lock = xSemaphoreCreateMutex();
    if (s_engine_lock == NULL)
    {
        ESP_LOGE(TAG, "No se pudo crear el lock de inicializacion del motor IR");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Motor IR RMT en modo bajo demanda (TX=GPIO%d, RX=GPIO%d): "
                  "arranca en el primer uso",
             CONFIG_ROBOT_IR_TX_GPIO, CONFIG_ROBOT_IR_RX_GPIO);
    return ESP_OK;
}
