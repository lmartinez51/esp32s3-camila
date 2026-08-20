/**
 * @file ble_hue.c
 * @brief Philips Hue Bluetooth Smart Light — Robot HAL driver implementation.
 *
 * Implements the Philips Hue BLE driver:
 *  - Ephemeral connections: connects on-demand, writes GATT payload, disconnects immediately.
 *  - PSRAM state cache: maintains last ON/OFF and brightness state per device to resolve
 *    ROBOT_ACTION_TOGGLE without prior GATT reads.
 *  - Bounded GATT writes (<= 1000 ms timeout) and fail-fast passive scan probe (<= 400 ms).
 *  - Zero Internal SRAM: all driver state context is allocated strictly in PSRAM.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ble_hue.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "ble_transport.h"
#include "robot_hal.h"

#define TAG "BLE_HUE"

#if !defined(CONFIG_ROBOT_PROBE_TIMEOUT_MS)
#define CONFIG_ROBOT_PROBE_TIMEOUT_MS 400
#endif

/* Nombre legible de un codigo de error NimBLE (numeracion del fork ESP-IDF,
 * ver host/ble_hs.h). El fork no provee ble_hs_err_str(). */
static const char *ble_hue_hs_err_name(int rc)
{
    switch (rc)
    {
    case 0: return "ENOERR";
    case 1: return "EAGAIN";
    case 2: return "EALREADY";
    case 3: return "EINVAL";
    case 4: return "EMSGSIZE";
    case 5: return "ENOENT";
    case 6: return "ENOMEM";
    case 7: return "ENOTCONN";
    case 8: return "ENOTSUP";
    case 9: return "EAPP";
    case 10: return "EBADDATA";
    case 11: return "EOS";
    case 12: return "ECONTROLLER";
    case 13: return "ETIMEOUT";
    case 14: return "EDONE";
    case 15: return "EBUSY";
    case 16: return "EREJECT";
    case 17: return "EUNKNOWN";
    case 18: return "EROLE";
    default: return "?";
    }
}

/* ── 128-bit UUID constants (little-endian for NimBLE) ───────────────── */

/* Service UUID: 932c32bd-de80-47a7-93ab-e652d82b7f1e */
static const ble_uuid128_t s_hue_service_uuid =
    BLE_UUID128_INIT(0x1e, 0x7f, 0x2b, 0xd8, 0x52, 0xe6, 0xab, 0x93,
                     0xa7, 0x47, 0x80, 0xde, 0xbd, 0x32, 0x2c, 0x93);

/* On/Off Characteristic UUID: be880001-eb94-4bc2-b590-e25d60909113 */
static const ble_uuid128_t s_hue_on_off_char_uuid =
    BLE_UUID128_INIT(0x13, 0x91, 0x90, 0x60, 0x5d, 0xe2, 0x90, 0xb5,
                     0xc2, 0x4b, 0x94, 0xeb, 0x01, 0x00, 0x88, 0xbe);

/* Brightness Characteristic UUID: be880002-eb94-4bc2-b590-e25d60909113 */
static const ble_uuid128_t s_hue_brightness_char_uuid =
    BLE_UUID128_INIT(0x13, 0x91, 0x90, 0x60, 0x5d, 0xe2, 0x90, 0xb5,
                     0xc2, 0x4b, 0x94, 0xeb, 0x02, 0x00, 0x88, 0xbe);

/* Protocolo Hue BLE canónico (focos Hue LCA006, verificados con nRF Connect
 * en 'Bathroom' y documentados en hueble/huec): servicio
 * 932c32bd-0000-47a2-835a-a8d455b859dd con writes de 1 byte:
 *   - 932c32bd-0002 (Light State)      = power    (0x00/0x01)
 *   - 932c32bd-0003 (State per Attribute) = brillo (0x00..0xFF) */
static const ble_uuid128_t s_hue_light_state_char_uuid =
    BLE_UUID128_INIT(0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83,
                     0xa2, 0x47, 0x02, 0x00, 0xbd, 0x32, 0x2c, 0x93);

static const ble_uuid128_t s_hue_light_state_attr_char_uuid =
    BLE_UUID128_INIT(0xdd, 0x59, 0xb8, 0x55, 0xd4, 0xa8, 0x5a, 0x83,
                     0xa2, 0x47, 0x03, 0x00, 0xbd, 0x32, 0x2c, 0x93);

/* Servicio vendor Philips Hue 0xFE0F (device_configuration_service_info /
 * ProximityPairingSetup): el char 97fe6561-2004 es la macro "make
 * discoverable". Escribir 0x01 pone al foco en modo emparejable (~1 min),
 * requisito de Philips para aceptar bonding SMP desde un master arbitrario
 * (fuentes: hueble.readthedocs.io, hue-ble-ctl, Nature Remo/OpenMQTTGateway). */
static const ble_uuid128_t s_hue_make_discoverable_char_uuid =
    BLE_UUID128_INIT(0x22, 0x3d, 0xda, 0xe2, 0x1e, 0xb7, 0xe9, 0x86,
                     0x62, 0x4f, 0x04, 0x20, 0x61, 0x65, 0xfe, 0x97);

/* ── Tuya Smart Bulb (protocolo BLE Tuya) ────────────────────────────── */

#define BLE_TUYA_SERVICE_UUID16    0x1912   /* Servicio de control Tuya         */
#define BLE_TUYA_WRITE_CHAR_UUID16 0x2AE2   /* Característica de comando (write) */
#define BLE_TUYA_DATA_CHAR_UUID16  0x2AE1   /* Característica de datos (notify)  */

typedef enum
{
    BLE_LIGHT_PROTO_UNKNOWN = 0,
    BLE_LIGHT_PROTO_HUE,   /* Hue BLE: 1 byte (932c32bd-0002/0003) o be8800xx */
    BLE_LIGHT_PROTO_TUYA,  /* trama 0x55AA + dp de control */
} ble_light_protocol_t;

/* ── PSRAM Driver Context ────────────────────────────────────────────── */

typedef struct
{
    char     alias[ROBOT_ALIAS_MAX_LEN];
    uint8_t  addr[6];
    bool     last_state;        /* true = ON, false = OFF */
    uint8_t  last_brightness;   /* 1..100 (%) */
    uint16_t on_off_handle;     /* Cached GATT value handle para Hue On/Off   */
    uint16_t brightness_handle; /* Cached GATT value handle para Hue Brillo   */
    uint16_t tuya_handle;       /* Cached GATT value handle para comando Tuya */
    uint16_t make_discoverable_handle; /* 97fe6561-2004 (pairing window)      */
    ble_light_protocol_t protocol;
    bool     handles_cached;
} ble_hue_device_state_t;

typedef struct
{
    ble_hue_device_state_t *devices; /* Allocated strictly in PSRAM */
    size_t                 device_count;
    SemaphoreHandle_t      mutex;
} ble_hue_driver_ctx_t;

static robot_driver_t s_ble_hue_driver;

/* ── Device state management in PSRAM ────────────────────────────────── */

static ble_hue_device_state_t *ble_hue_get_or_create_device_state(ble_hue_driver_ctx_t *ctx,
                                                                 const char *alias,
                                                                 const uint8_t addr[6])
{
    if (ctx == NULL || ctx->devices == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < ctx->device_count; i++)
    {
        if ((alias != NULL && strcmp(ctx->devices[i].alias, alias) == 0) ||
            (addr != NULL && memcmp(ctx->devices[i].addr, addr, 6) == 0))
        {
            return &ctx->devices[i];
        }
    }

    if (ctx->device_count >= ROBOT_REGISTRY_MAX_DEVICES)
    {
        return NULL;
    }

    ble_hue_device_state_t *st = &ctx->devices[ctx->device_count++];
    memset(st, 0, sizeof(*st));
    if (alias != NULL)
    {
        strlcpy(st->alias, alias, sizeof(st->alias));
    }
    if (addr != NULL)
    {
        memcpy(st->addr, addr, 6);
    }
    st->last_state = false;
    st->last_brightness = 100;
    st->handles_cached = false;
    return st;
}

/* ── Characteristic discovery ────────────────────────────────────────── */

typedef struct
{
    uint16_t          on_off_handle;
    uint16_t          brightness_handle;
    uint16_t          tuya_handle;
    uint16_t          make_discoverable_handle;
    ble_light_protocol_t protocol;
    SemaphoreHandle_t sem;
} hue_disc_ctx_t;

static int ble_hue_on_chr_disc(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr,
                               void *arg)
{
    (void)conn_handle;
    hue_disc_ctx_t *ctx = (hue_disc_ctx_t *)arg;
    if (ctx == NULL)
    {
        return 0;
    }

    if (error->status == BLE_HS_EDONE || error->status != 0)
    {
        if (ctx->sem != NULL)
        {
            xSemaphoreGive(ctx->sem);
        }
        return 0;
    }

    {
        char uuid_str[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&chr->uuid.u, uuid_str);
        ESP_LOGD(TAG, "  [disc] Char: %s (handle 0x%04X, props 0x%02X)", uuid_str, chr->val_handle, chr->properties);
    }

    if (chr->uuid.u.type == BLE_UUID_TYPE_128)
    {
        if (ble_uuid_cmp(&chr->uuid.u, &s_hue_on_off_char_uuid.u) == 0)
        {
            ctx->on_off_handle = chr->val_handle;
            ctx->protocol = BLE_LIGHT_PROTO_HUE;
            ESP_LOGD(TAG, "Philips Hue On/Off characteristic handle: 0x%04X", chr->val_handle);
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &s_hue_brightness_char_uuid.u) == 0)
        {
            ctx->brightness_handle = chr->val_handle;
            ctx->protocol = BLE_LIGHT_PROTO_HUE;
            ESP_LOGD(TAG, "Philips Hue Brightness characteristic handle: 0x%04X", chr->val_handle);
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &s_hue_light_state_char_uuid.u) == 0)
        {
            /* Hue BLE canónico: 932c32bd-0002 = power (1 byte 0x00/0x01) */
            if (ctx->on_off_handle == 0)
            {
                ctx->on_off_handle = chr->val_handle;
                ctx->protocol = BLE_LIGHT_PROTO_HUE;
                ESP_LOGD(TAG, "Hue Light State (power) characteristic handle: 0x%04X", chr->val_handle);
            }
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &s_hue_light_state_attr_char_uuid.u) == 0)
        {
            /* Hue BLE canónico: 932c32bd-0003 = brillo (1 byte 0x00..0xFF) */
            if (ctx->brightness_handle == 0)
            {
                ctx->brightness_handle = chr->val_handle;
                ctx->protocol = BLE_LIGHT_PROTO_HUE;
                ESP_LOGD(TAG, "Hue Light State (brightness) characteristic handle: 0x%04X", chr->val_handle);
            }
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &s_hue_make_discoverable_char_uuid.u) == 0)
        {
            /* Macro "make discoverable": write 0x01 abre la ventana de
             * emparejamiento (~1 min) necesaria para el bonding SMP. */
            ctx->make_discoverable_handle = chr->val_handle;
            ESP_LOGD(TAG, "Hue make-discoverable characteristic handle: 0x%04X", chr->val_handle);
        }
    }
    else if (chr->uuid.u.type == BLE_UUID_TYPE_16)
    {
        uint16_t uuid16 = BLE_UUID16(&chr->uuid.u)->value;
        if (uuid16 == BLE_TUYA_WRITE_CHAR_UUID16 && ctx->tuya_handle == 0)
        {
            ctx->tuya_handle = chr->val_handle;
            ctx->protocol = BLE_LIGHT_PROTO_TUYA;
            ESP_LOGD(TAG, "Tuya command characteristic handle: 0x%04X", chr->val_handle);
        }
    }
    return 0;
}

/* ── GATT Write ──────────────────────────────────────────────────────── */

typedef struct
{
    SemaphoreHandle_t sem;
    int               status; /* 0 = OK; otro = error ATT reportado por NimBLE */
} hue_write_ack_t;

static int ble_hue_on_write_cb(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr,
                               void *arg)
{
    (void)conn_handle;
    (void)attr;
    hue_write_ack_t *ack = (hue_write_ack_t *)arg;
    if (ack != NULL)
    {
        ack->status = (error != NULL) ? error->status : 0;
        if (ack->status != 0)
        {
            ESP_LOGW(TAG, "GATT write status: %d", ack->status);
        }
        xSemaphoreGive(ack->sem);
    }
    return 0;
}

/* ── Tuya BLE frame ──────────────────────────────────────────────────── */

static uint16_t s_tuya_seq = 0;

/**
 * @brief Construye una trama de control Tuya (cmd 0x02) para una luz.
 *
 * Formato: 55 AA <seq:2 LE> <cmd=0x02> <len:2 LE> <dp_id> <dp_type> <dp_len>
 *          <dp_val...> <crc>
 * CRC = XOR de todos los bytes posteriores al header 0x55 0xAA.
 */
static uint16_t ble_hue_build_tuya_frame(uint8_t *buf, size_t cap,
                                         uint8_t dp_id, uint8_t dp_type,
                                         const uint8_t *dp_val, uint8_t dp_len)
{
    if (buf == NULL || cap < 9u + dp_len)
    {
        return 0;
    }

    uint16_t seq = s_tuya_seq++;
    size_t i = 0;
    buf[i++] = 0x55;
    buf[i++] = 0xAA;
    buf[i++] = (uint8_t)(seq & 0xFF);
    buf[i++] = (uint8_t)((seq >> 8) & 0xFF);
    buf[i++] = 0x02; /* cmd: control/query DP */
    buf[i++] = (uint8_t)((dp_len + 3u) & 0xFF);  /* len: dp_id + dp_type + dp_len + dp_val */
    buf[i++] = (uint8_t)(((dp_len + 3u) >> 8) & 0xFF);
    buf[i++] = dp_id;
    buf[i++] = dp_type;
    buf[i++] = dp_len;
    for (uint8_t k = 0; k < dp_len; k++)
    {
        buf[i++] = dp_val[k];
    }

    uint8_t crc = 0;
    for (size_t k = 2; k < i; k++)
    {
        crc ^= buf[k];
    }
    buf[i++] = crc;

    return (uint16_t)i;
}

static esp_err_t ble_hue_write_char(uint16_t conn_handle, uint16_t char_handle,
                                    const uint8_t *data, uint16_t len,
                                    int *out_att_status)
{
    if (out_att_status)
    {
        *out_att_status = 0;
    }
    if (conn_handle == 0 || char_handle == 0 || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    hue_write_ack_t ack = { .sem = xSemaphoreCreateBinary(), .status = -1 };
    if (ack.sem == NULL)
    {
        /* Fallback: write without response */
        int rc = ble_gattc_write_no_rsp_flat(conn_handle, char_handle, data, len);
        return (rc == 0) ? ESP_OK : ESP_FAIL;
    }

    /* Bounded write with timeout <= 1000 ms */
    int rc = ble_gattc_write_flat(conn_handle, char_handle, data, len,
                                  ble_hue_on_write_cb, (void *)&ack);
    if (rc != 0)
    {
        /* No se pudo iniciar el write con respuesta: reintentar sin respuesta */
        vSemaphoreDelete(ack.sem);
        rc = ble_gattc_write_no_rsp_flat(conn_handle, char_handle, data, len);
        return (rc == 0) ? ESP_OK : ESP_FAIL;
    }

    bool acked = (xSemaphoreTake(ack.sem, pdMS_TO_TICKS(1000)) == pdTRUE);
    vSemaphoreDelete(ack.sem);

    if (!acked)
    {
        ESP_LOGW(TAG, "GATT write timeout (1000 ms) en handle 0x%04X", char_handle);
        return ESP_ERR_TIMEOUT;
    }

    /* El peer puede rechazar el write (p. ej. Write Not Permitted o
     * Insufficient Authentication/Encryption). Propagar el error para no
     * reportar falsos éxitos al chatbot. */
    if (ack.status != 0)
    {
        if (out_att_status)
        {
            *out_att_status = ack.status;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Driver Lifecycle ────────────────────────────────────────────────── */

static esp_err_t ble_hue_init(robot_driver_t *drv)
{
    if (drv == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (drv->priv != NULL)
    {
        return ESP_OK; /* Idempotent */
    }

    ble_hue_driver_ctx_t *ctx = heap_caps_malloc(
        sizeof(ble_hue_driver_ctx_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria en PSRAM para contexto ble_hue");
        return ESP_ERR_NO_MEM;
    }
    memset(ctx, 0, sizeof(*ctx));

    ctx->devices = heap_caps_malloc(
        sizeof(ble_hue_device_state_t) * ROBOT_REGISTRY_MAX_DEVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->devices == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria en PSRAM para estados de dispositivos ble_hue");
        heap_caps_free(ctx);
        return ESP_ERR_NO_MEM;
    }
    memset(ctx->devices, 0, sizeof(ble_hue_device_state_t) * ROBOT_REGISTRY_MAX_DEVICES);

    ctx->mutex = xSemaphoreCreateMutex();
    if (ctx->mutex == NULL)
    {
        heap_caps_free(ctx->devices);
        heap_caps_free(ctx);
        return ESP_ERR_NO_MEM;
    }

    drv->priv = ctx;
    ESP_LOGI(TAG, "Driver ble_hue inicializado (PSRAM)");
    return ESP_OK;
}

static esp_err_t ble_hue_probe(robot_driver_t *drv,
                               const robot_endpoint_t *ep,
                               bool *present)
{
    (void)drv;
    (void)ep;
    if (!present)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Bypass passive scan pre-probe for BLE lights.
     * Philips Hue advertising interval is 1000–1500 ms (or higher in standby),
     * which exceeds the 400 ms scan window. We bypass passive pre-probe and
     * proceed directly to bounded connect (1500 ms) in execute(). */
    *present = true;
    return ESP_OK;
}

static esp_err_t ble_hue_execute(robot_driver_t *drv,
                                 const char *alias,
                                 robot_action_id_t action,
                                 const robot_action_params_t *params,
                                 robot_result_t *out)
{
    if (!alias || !out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->code = ROBOT_RESULT_ERR_TRANSPORT;

    if ((BLE_HUE_CAP_MASK & (1u << action)) == 0)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Accion '%s' no soportada por '%s'",
                 robot_action_to_string(action), BLE_HUE_PROFILE_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ble_hue_driver_ctx_t *ctx = (ble_hue_driver_ctx_t *)drv->priv;
    if (ctx == NULL)
    {
        esp_err_t init_err = ble_hue_init(drv);
        if (init_err != ESP_OK)
        {
            out->code = ROBOT_RESULT_ERR_TRANSPORT;
            snprintf(out->detail, sizeof(out->detail), "Fallo inicializando contexto ble_hue");
            return init_err;
        }
        ctx = (ble_hue_driver_ctx_t *)drv->priv;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        out->code = ROBOT_RESULT_ERR_NOT_FOUND;
        snprintf(out->detail, sizeof(out->detail), "Dispositivo '%s' no registrado", alias);
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ble_hue_device_state_t *st = ble_hue_get_or_create_device_state(ctx, alias, dev->endpoint.addr);
    if (st == NULL)
    {
        xSemaphoreGive(ctx->mutex);
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "Sin memoria para estado de dispositivo '%s'", alias);
        return ESP_ERR_NO_MEM;
    }

    /* 1. Resolve action and payload */
    robot_action_id_t eff_action = action;
    if (action == ROBOT_ACTION_TOGGLE)
    {
        eff_action = st->last_state ? ROBOT_ACTION_TURN_OFF : ROBOT_ACTION_TURN_ON;
        ESP_LOGD(TAG, "TOGGLE resuelto a %s (last_state=%s)",
                 robot_action_to_string(eff_action), st->last_state ? "ON" : "OFF");
    }

    uint8_t payload_byte = 0;
    bool is_brightness = (eff_action == ROBOT_ACTION_SET_BRIGHTNESS);
    if (eff_action == ROBOT_ACTION_TURN_ON)
    {
        payload_byte = 0x01;
    }
    else if (eff_action == ROBOT_ACTION_TURN_OFF)
    {
        payload_byte = 0x00;
    }
    else if (is_brightness)
    {
        uint8_t pct = (params != NULL && params->brightness_pct > 0) ? params->brightness_pct : 100;
        if (pct > 100) pct = 100;
        if (pct < 1) pct = 1;
        /* Linear map 1..100% -> 0x01..0xFE (1..254) */
        payload_byte = (uint8_t)(1 + ((uint32_t)(pct - 1) * 253u) / 99u);
        st->last_brightness = pct;
    }

    /* 2. Direct Bounded Connect (1500 ms Phase 1 timeout, 2500 ms Phase 2 GATT) */
    uint16_t conn_handle = 0;
    uint16_t unused_char_handle = 0;
    esp_err_t err = ble_transport_connect_and_wait(dev->endpoint.addr, dev->endpoint.addr_type,
                                                   1500, /* 1500 ms bounded connection timeout */
                                                   2500, /* 2500 ms GATT discovery timeout */
                                                   &conn_handle, &unused_char_handle);
    if (err != ESP_OK || conn_handle == 0)
    {
        xSemaphoreGive(ctx->mutex);
        out->code = (err == ESP_ERR_TIMEOUT) ? ROBOT_RESULT_ERR_TIMEOUT : ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "Fallo conectando a '%s': %s",
                 alias, esp_err_to_name(err));
        return err;
    }

    /* 3. Discover characteristics if handles are not yet cached */
    if (!st->handles_cached && dev->endpoint.value_handle > 0)
    {
        st->on_off_handle = dev->endpoint.value_handle;
        st->handles_cached = true;
    }
    if (!st->handles_cached && unused_char_handle > 0)
    {
        st->on_off_handle = unused_char_handle;
        st->handles_cached = true;
    }

    bool need_disc = !st->handles_cached ||
                     (is_brightness && st->brightness_handle == 0) ||
                     (st->protocol == BLE_LIGHT_PROTO_TUYA && st->tuya_handle == 0) ||
                     (st->protocol != BLE_LIGHT_PROTO_TUYA && st->on_off_handle == 0);
    if (need_disc)
    {
        hue_disc_ctx_t disc = {0};
        disc.sem = xSemaphoreCreateBinary();
        if (disc.sem != NULL)
        {
            int rc = ble_gattc_disc_all_chrs(conn_handle, 1, 0xffff, ble_hue_on_chr_disc, &disc);
            if (rc == 0)
            {
                xSemaphoreTake(disc.sem, pdMS_TO_TICKS(1500));
                if (disc.on_off_handle > 0) st->on_off_handle = disc.on_off_handle;
                if (disc.brightness_handle > 0) st->brightness_handle = disc.brightness_handle;
                if (disc.tuya_handle > 0) st->tuya_handle = disc.tuya_handle;
                if (disc.make_discoverable_handle > 0) st->make_discoverable_handle = disc.make_discoverable_handle;
                if (disc.protocol != BLE_LIGHT_PROTO_UNKNOWN) st->protocol = disc.protocol;
                if (st->on_off_handle > 0 || st->tuya_handle > 0) st->handles_cached = true;
            }
            vSemaphoreDelete(disc.sem);
        }
    }

    /* Target handle y payload según el protocolo detectado */
    uint16_t target_handle = 0;
    uint8_t payload[16];
    uint16_t payload_len = 0;

    if (st->protocol == BLE_LIGHT_PROTO_TUYA && st->tuya_handle > 0)
    {
        target_handle = st->tuya_handle;

        uint8_t dp_val[4] = {0};
        uint8_t dp_id, dp_type, dp_len;
        if (is_brightness)
        {
            /* Brillo Tuya: dp 0x02, tipo value (0x02), 4 bytes BE = pct*10 (0..1000) */
            uint32_t bv = (uint32_t)st->last_brightness * 10u;
            dp_val[0] = (uint8_t)(bv >> 24);
            dp_val[1] = (uint8_t)(bv >> 16);
            dp_val[2] = (uint8_t)(bv >> 8);
            dp_val[3] = (uint8_t)(bv & 0xFF);
            dp_id = 0x02;
            dp_type = 0x02;
            dp_len = 4;
        }
        else
        {
            /* On/Off Tuya: dp 0x01, tipo bool (0x01), 1 byte 0x00/0x01 */
            dp_val[0] = payload_byte;
            dp_id = 0x01;
            dp_type = 0x01;
            dp_len = 1;
        }
        payload_len = ble_hue_build_tuya_frame(payload, sizeof(payload), dp_id, dp_type, dp_val, dp_len);
        if (payload_len == 0)
        {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            xSemaphoreGive(ctx->mutex);
            out->code = ROBOT_RESULT_ERR_TRANSPORT;
            snprintf(out->detail, sizeof(out->detail), "Error construyendo trama Tuya para '%s'", alias);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Tuya frame (%u B) -> handle 0x%04X, dp=%u", payload_len, target_handle, dp_id);
    }
    else
    {
        target_handle = is_brightness ? st->brightness_handle : st->on_off_handle;
        payload[0] = payload_byte;
        payload_len = 1;
    }

    /* Fallback al handle aprendido por el profile discovery */
    if (target_handle == 0)
    {
        target_handle = (dev->endpoint.value_handle > 0) ? dev->endpoint.value_handle : unused_char_handle;

        /* Protocolo no identificado (p. ej. foco 0xFE0F con chars 0xFFF1/0xFFF2):
         * usar trama Tuya 0x55AA, la más probable en focos controlados por la
         * app Smart Life/Tuya. Si el foco no reacciona, ajustar protocolo. */
        if (target_handle > 0 && st->protocol == BLE_LIGHT_PROTO_UNKNOWN)
        {
            uint8_t dp_val[4] = {0};
            uint8_t dp_id, dp_type, dp_len;
            if (is_brightness)
            {
                uint32_t bv = (uint32_t)st->last_brightness * 10u;
                dp_val[0] = (uint8_t)(bv >> 24);
                dp_val[1] = (uint8_t)(bv >> 16);
                dp_val[2] = (uint8_t)(bv >> 8);
                dp_val[3] = (uint8_t)(bv & 0xFF);
                dp_id = 0x02; dp_type = 0x02; dp_len = 4;
            }
            else
            {
                dp_val[0] = payload_byte;
                dp_id = 0x01; dp_type = 0x01; dp_len = 1;
            }
            payload_len = ble_hue_build_tuya_frame(payload, sizeof(payload), dp_id, dp_type, dp_val, dp_len);
            if (payload_len > 0)
            {
                ESP_LOGW(TAG, "Protocolo no identificado en '%s': enviando trama Tuya (%u B) a 0x%04X (verificar reacción del foco)", alias, payload_len, target_handle);
            }
            else
            {
                payload[0] = payload_byte;
                payload_len = 1;
            }
        }
    }

    if (target_handle == 0)
    {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreGive(ctx->mutex);
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        snprintf(out->detail, sizeof(out->detail), "No se encontro handle GATT para '%s'", alias);
        return ESP_FAIL;
    }

    /* 4. Write characteristic with bounded timeout <= 1000 ms. Algunos focos
     * Hue-compatibles exigen vínculo cifrado: si el write falla con
     * Insufficient Authentication (0x105 / 261) o Insufficient Encryption (0x10F),
     * se abre la ventana de emparejamiento y se inicia seguridad SMP IN-PLACE sobre
     * la conexión activa (sin desconectar ni reconectar). */
    int att_status = 0;
    char pairing_fail[256] = "";
    err = ble_hue_write_char(conn_handle, target_handle, payload, payload_len, &att_status);
    if (err != ESP_OK && (att_status == 0x105 || att_status == 0x10F || att_status == 261))
    {
        ESP_LOGW(TAG, "El foco '%s' exige vínculo cifrado (ATT 0x%03X / %d); iniciando seguridad SMP in-place...",
                 alias, att_status, att_status);

        /* Si make-discoverable está disponible, abrir la ventana */
        if (st->make_discoverable_handle > 0)
        {
            uint8_t md = 0x01;
            int md_status = 0;
            esp_err_t md_err = ble_hue_write_char(conn_handle, st->make_discoverable_handle,
                                                  &md, 1, &md_status);
            if (md_err != ESP_OK)
            {
                ESP_LOGW(TAG, "make-discoverable con respuesta falló (ATT 0x%03X); probando Write-Without-Response...",
                         md_status);
                int rc = ble_gattc_write_no_rsp_flat(conn_handle, st->make_discoverable_handle, &md, 1);
                ESP_LOGI(TAG, "Hue make-discoverable no_rsp (0x01 -> 0x%04X): rc=%d",
                         st->make_discoverable_handle, rc);
            }
            else
            {
                ESP_LOGI(TAG, "Hue make-discoverable (0x01 -> 0x%04X): OK", st->make_discoverable_handle);
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        /* Iniciar seguridad/cifrado SMP IN-PLACE directamente sobre el enlace activo */
        int rc_sec = ble_gap_security_initiate(conn_handle);

        bool encrypted = false;
        if (rc_sec != 0)
        {
            ESP_LOGW(TAG, "ble_gap_security_initiate in-place fallo con rc=%d (%s)",
                     rc_sec, ble_hue_hs_err_name(rc_sec));
            snprintf(pairing_fail, sizeof(pairing_fail),
                     "no se pudo iniciar el emparejamiento (NimBLE rc=%d: %s).",
                     rc_sec, ble_hue_hs_err_name(rc_sec));
        }
        else
        {
            ESP_LOGI(TAG, "ble_gap_security_initiate in-place rc=0; esperando cifrado...");
            for (int i = 0; i < 48; i++) /* hasta ~1200 ms */
            {
                vTaskDelay(pdMS_TO_TICKS(25));
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(conn_handle, &desc) != 0)
                {
                    break; /* enlace perdido durante SMP */
                }
                if (desc.sec_state.encrypted)
                {
                    encrypted = true;
                    break;
                }
            }
        }

        if (encrypted)
        {
            /* Esperar a que se asiente el enlace cifrado */
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "Enlace cifrado in-place exitoso; reintentando escritura GATT en handle 0x%04X...", target_handle);
            att_status = 0;
            err = ble_hue_write_char(conn_handle, target_handle, payload, payload_len, &att_status);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Primer reintento falló con ATT status 0x%03X; reintentando tras 100 ms...", att_status);
                vTaskDelay(pdMS_TO_TICKS(100));
                att_status = 0;
                err = ble_hue_write_char(conn_handle, target_handle, payload, payload_len, &att_status);
            }
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "✅ Reintento de escritura tras cifrado in-place exitoso en 0x%04X", target_handle);
            }
        }
        else if (pairing_fail[0] == '\0')
        {
            ESP_LOGW(TAG, "No se logro cifrar el enlace in-place con '%s'.", alias);
            snprintf(pairing_fail, sizeof(pairing_fail),
                     "el foco '%s' rechazó el emparejamiento SMP.", alias);
            err = ESP_FAIL;
        }
    }

    /* 5. Ephemeral Connection: Disconnect immediately to free radio and NimBLE buffers */
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);

    if (err == ESP_OK)
    {
        if (eff_action == ROBOT_ACTION_TURN_ON)
        {
            st->last_state = true;
        }
        else if (eff_action == ROBOT_ACTION_TURN_OFF)
        {
            st->last_state = false;
        }
        else if (is_brightness)
        {
            st->last_state = true;
        }
    }

    xSemaphoreGive(ctx->mutex);

    if (err != ESP_OK)
    {
        out->code = (err == ESP_ERR_TIMEOUT) ? ROBOT_RESULT_ERR_TIMEOUT : ROBOT_RESULT_ERR_TRANSPORT;
        if (pairing_fail[0] != '\0')
        {
            snprintf(out->detail, sizeof(out->detail),
                     "NO se pudo ejecutar la accion en la luz '%s': %s La luz no cambio de estado.",
                     alias, pairing_fail);
        }
        else
        {
            snprintf(out->detail, sizeof(out->detail), "Escritura GATT fallida en '%s': %s",
                     alias, esp_err_to_name(err));
        }
        return err;
    }

    out->code = ROBOT_RESULT_OK;
    if (eff_action == ROBOT_ACTION_TURN_ON)
    {
        snprintf(out->detail, sizeof(out->detail), "Luz '%s' encendida", alias);
    }
    else if (eff_action == ROBOT_ACTION_TURN_OFF)
    {
        snprintf(out->detail, sizeof(out->detail), "Luz '%s' apagada", alias);
    }
    else if (is_brightness)
    {
        snprintf(out->detail, sizeof(out->detail), "Brillo de '%s' ajustado al %u%%",
                 alias, (unsigned)st->last_brightness);
    }
    else
    {
        snprintf(out->detail, sizeof(out->detail), "Comando de luz ejecutado en '%s'", alias);
    }

    return ESP_OK;
}

static robot_driver_t s_ble_hue_driver = {
    .profile_id   = BLE_HUE_PROFILE_ID,
    .category     = ROBOT_CATEGORY_LIGHT,
    .protocol     = ROBOT_PROTOCOL_BLE,
    .capabilities = BLE_HUE_CAP_MASK,
    .init         = ble_hue_init,
    .execute      = ble_hue_execute,
    .probe        = ble_hue_probe,
    .deinit       = NULL,
    .priv         = NULL,
};

const robot_driver_t *ble_hue_get_driver(void)
{
    return &s_ble_hue_driver;
}

const robot_driver_t *ble_hue_driver_init(void)
{
    ble_hue_init(&s_ble_hue_driver);
    return &s_ble_hue_driver;
}
