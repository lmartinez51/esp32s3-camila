/**
 * @file ble_transport.c
 * @brief Shared BLE central transport — probe, connect policy, raw I/O (Phase 3).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "ble_transport.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "ble_common.h"
#include "ble_device_control.h"

#define TAG "BLE_TRANSPORT"

#define BLE_TRANSPORT_STOP_DATA_MAX 16u
#define BLE_TRANSPORT_PULSE_TASK_STACK 3072u

/* ── Bounded passive scan probe ────────────────────────────────────────── */

typedef struct
{
    const uint8_t *addr;    /* MAC match (NULL -> name match) */
    uint8_t addr_type;
    const char *name;       /* substring match when addr == NULL */
    bool found;
    SemaphoreHandle_t done;
} ble_transport_probe_ctx_t;

static int ble_transport_probe_event_handler(struct ble_gap_event *event, void *arg)
{
    ble_transport_probe_ctx_t *ctx = (ble_transport_probe_ctx_t *)arg;
    if (ctx == NULL)
    {
        return 0;
    }

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
    {
        bool match = false;
        if (ctx->addr != NULL)
        {
            match = (memcmp(event->disc.addr.val, ctx->addr, 6) == 0);
        }
        else if (ctx->name != NULL && ctx->name[0] != '\0')
        {
            struct ble_hs_adv_fields fields;
            char adv_name[BLE_DEVICE_MAX_NAME_LEN] = {0};
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0 &&
                fields.name != NULL)
            {
                const size_t name_len = (fields.name_len < sizeof(adv_name) - 1) ? fields.name_len
                                                                                 : sizeof(adv_name) - 1;
                memcpy(adv_name, fields.name, name_len);
                adv_name[name_len] = '\0';
            }
            match = (adv_name[0] != '\0' && strstr(adv_name, ctx->name) != NULL);
        }

        if (match && !ctx->found)
        {
            ctx->found = true;
            ESP_LOGI(TAG, "Probe: dispositivo visto en escaneo pasivo");
            ble_gap_disc_cancel(); /* DISC_COMPLETE se postea y libera la semafora */
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (ctx->done != NULL)
        {
            xSemaphoreGive(ctx->done);
        }
        break;

    default:
        break;
    }

    return 0;
}

static esp_err_t ble_transport_probe_scan(const uint8_t *addr, uint8_t addr_type,
                                          const char *name, uint32_t timeout_ms,
                                          bool *present)
{
    if (present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;

    if (!ble_common_is_synced())
    {
        ESP_LOGW(TAG, "Probe omitido: host NimBLE no sincronizado");
        return ESP_ERR_INVALID_STATE;
    }

    ble_transport_probe_ctx_t ctx = {0};
    ctx.addr = addr;
    ctx.addr_type = addr_type;
    ctx.name = name;

    ctx.done = xSemaphoreCreateBinary();
    if (ctx.done == NULL)
    {
        ESP_LOGE(TAG, "Probe: sin memoria para semaforo");
        return ESP_ERR_NO_MEM;
    }

    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 0;
    params.passive = 1;
    params.itvl = 0x0080;  /* ~128 ms */
    params.window = 0x0080;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, timeout_ms, &params,
                          ble_transport_probe_event_handler, &ctx);
    if (rc == BLE_HS_EBUSY)
    {
        /* Otra exploracion activa (p. ej. ciclo de descubrimiento del modulo
         * legado): degradar a "asumir presente" en vez de interferir. */
        ESP_LOGI(TAG, "Probe degradado: exploracion ya activa (EBUSY)");
        *present = true;
        vSemaphoreDelete(ctx.done);
        return ESP_OK;
    }
    if (rc != 0)
    {
        ESP_LOGW(TAG, "Probe: ble_gap_disc fallo rc=%d", rc);
        vSemaphoreDelete(ctx.done);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        /* Presupuesto agotado: cancelar para no dejar la radio escaneando. */
        ble_gap_disc_cancel();
        xSemaphoreTake(ctx.done, pdMS_TO_TICKS(200)); /* drenar DISC_COMPLETE */
    }

    *present = ctx.found;
    vSemaphoreDelete(ctx.done);
    ESP_LOGI(TAG, "Probe finalizado: %s", ctx.found ? "PRESENTE" : "AUSENTE");
    return ESP_OK;
}

esp_err_t ble_transport_probe_addr(const uint8_t addr[6], uint8_t addr_type,
                                   uint32_t timeout_ms, bool *present)
{
    if (addr == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ble_transport_probe_scan(addr, addr_type, NULL, timeout_ms, present);
}

esp_err_t ble_transport_probe_name(const char *name, uint32_t timeout_ms, bool *present)
{
    if (name == NULL || name[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ble_transport_probe_scan(NULL, 0, name, timeout_ms, present);
}

/* ── Phased-wait connect policy (reader over legacy state) ─────────────── */

static bool ble_transport_snapshot_device(const uint8_t addr[6], ble_device_info_t *out)
{
    ble_device_info_t *devices = heap_caps_malloc(
        sizeof(ble_device_info_t) * BLE_DEVICE_MAX_DEVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (devices == NULL)
    {
        ESP_LOGE(TAG, "connect: sin memoria para snapshot de dispositivos");
        return false;
    }

    bool found = false;
    const int count = ble_device_get_discovered_list(devices, BLE_DEVICE_MAX_DEVICES);
    for (int i = 0; i < count; i++)
    {
        if (memcmp(devices[i].addr, addr, 6) == 0)
        {
            memcpy(out, &devices[i], sizeof(ble_device_info_t));
            found = true;
            break;
        }
    }
    heap_caps_free(devices);
    return found;
}

esp_err_t ble_transport_connect_and_wait(const uint8_t addr[6], uint8_t addr_type,
                                         uint32_t phase1_timeout_ms,
                                         uint32_t phase2_timeout_ms,
                                         uint16_t *out_conn_handle,
                                         uint16_t *out_char_handle)
{
    if (addr == NULL || out_conn_handle == NULL || out_char_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_conn_handle = 0;
    *out_char_handle = 0;

    const uint32_t eff_phase1_timeout_ms = (phase1_timeout_ms > 0 && phase1_timeout_ms <= BLE_TRANSPORT_PHASE1_TIMEOUT_MS)
                                           ? phase1_timeout_ms
                                           : BLE_TRANSPORT_PHASE1_TIMEOUT_MS;
    const uint32_t eff_phase2_timeout_ms = (phase2_timeout_ms > 0)
                                           ? phase2_timeout_ms
                                           : BLE_TRANSPORT_PHASE2_TIMEOUT_MS;

    uint16_t conn_handle = 0;
    uint16_t char_handle = 0;
    bool conn_success = false;

    for (int attempt = 1; attempt <= (int)BLE_TRANSPORT_MAX_RETRIES; attempt++)
    {
        ble_device_info_t dev = {0};
        char dev_name[BLE_DEVICE_MAX_NAME_LEN] = "device";
        if (ble_transport_snapshot_device(addr, &dev))
        {
            if (dev.name[0] != '\0')
            {
                strlcpy(dev_name, dev.name, sizeof(dev_name));
            }
            /* Reutilizar conexion ya lista (el legado ya la descubrio). */
            if (dev.conn_handle != BLE_HS_CONN_HANDLE_NONE && dev.conn_handle != 0 &&
                dev.char_discovered && dev.char_val_handle > 0 &&
                (dev.state == BLE_DEVICE_STATE_CONNECTED || dev.state == BLE_DEVICE_STATE_DISCOVERY_COMPLETE))
            {
                *out_conn_handle = dev.conn_handle;
                *out_char_handle = dev.char_val_handle;
                ESP_LOGI(TAG, "connect: conexion existente reutilizada para '%s' (handle %u, char 0x%04X)",
                         dev_name, dev.conn_handle, dev.char_val_handle);
                return ESP_OK;
            }
        }
        else
        {
            ESP_LOGI(TAG, "connect: MAC %02X:%02X:%02X:%02X:%02X:%02X no esta en snapshot; iniciando conexion on-demand",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            memcpy(dev.addr, addr, 6);
            dev.addr_type = addr_type;
            dev.state = BLE_DEVICE_STATE_DISCONNECTED;
        }

        if (attempt > 1)
        {
            ESP_LOGW(TAG, "🔄 Retrying BLE transport connection to '%s' (attempt %d/%d)...",
                     dev_name, attempt, (int)BLE_TRANSPORT_MAX_RETRIES);
        }
        else
        {
            ESP_LOGI(TAG, "connect: iniciando intento %d/%d para '%s'",
                     attempt, (int)BLE_TRANSPORT_MAX_RETRIES, dev_name);
        }

        /* Iniciar conexion si no hay un intento en curso. */
        if (dev.state != BLE_DEVICE_STATE_CONNECTING)
        {
            esp_err_t rc = ble_device_connect(dev.addr, dev.addr_type);
            if (rc != ESP_OK)
            {
                ESP_LOGW(TAG, "connect: ble_device_connect devolvio %s para '%s' (intento %d/%d)",
                         esp_err_to_name(rc), dev_name, attempt, (int)BLE_TRANSPORT_MAX_RETRIES);
                if (attempt < (int)BLE_TRANSPORT_MAX_RETRIES)
                {
                    vTaskDelay(pdMS_TO_TICKS(BLE_TRANSPORT_RETRY_BACKOFF_MS));
                    continue;
                }
                return rc;
            }
        }
        else
        {
            ESP_LOGI(TAG, "connect: intento ya en curso, entrando en espera por fases");
        }

        /* Espera por fases (politica C8): abortar si el estado sale de CONNECTING
         * sin enlace (robot apagado) o si se agota cada presupuesto. */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t phase1_deadline = now_ms + eff_phase1_timeout_ms;
        uint32_t phase2_deadline = 0;
        bool linked = false;
        bool attempt_failed = false;

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

            ble_device_info_t snap = {0};
            if (ble_transport_snapshot_device(addr, &snap))
            {
                if (snap.conn_handle != BLE_HS_CONN_HANDLE_NONE && snap.conn_handle != 0)
                {
                    conn_handle = snap.conn_handle;
                    if (!linked)
                    {
                        linked = true;
                        phase2_deadline = now + eff_phase2_timeout_ms;

                        /* GATT Caching Fast-Path:
                         * Si el dispositivo ya tiene handle en caché,
                         * saltar la espera de descubrimiento GATT (Fase 2) y retornar inmediatamente. */
                        uint16_t cached_handle = (snap.char_val_handle > 0) ? snap.char_val_handle : dev.char_val_handle;
                        if (cached_handle > 0)
                        {
                            char_handle = cached_handle;
                            conn_success = true;
                            ESP_LOGI(TAG, "⚡ Fast-Path: Reusing cached GATT handle 0x%04X for '%s'",
                                     cached_handle, dev_name);
                            break;
                        }
                    }
                    if (snap.char_discovered && snap.char_val_handle > 0 &&
                        (snap.state == BLE_DEVICE_STATE_CONNECTED || snap.state == BLE_DEVICE_STATE_DISCOVERY_COMPLETE))
                    {
                        char_handle = snap.char_val_handle;
                        conn_success = true;
                    }
                }
                else if (linked)
                {
                    ESP_LOGW(TAG, "connect: el enlace con '%s' se cayo durante la espera GATT", dev_name);
                    linked = false;
                }
                else if (snap.state != BLE_DEVICE_STATE_CONNECTING)
                {
                    /* El intento ya termino sin enlace (error/desconexion):
                     * abortar de inmediato en lugar de agotar la fase 1. */
                    ESP_LOGW(TAG, "connect: intento %d con '%s' termino sin enlace (estado %d). Abortando.",
                             attempt, dev_name, (int)snap.state);
                    attempt_failed = true;
                }
            }

            if (attempt_failed || conn_success)
            {
                break;
            }

            if (linked && phase2_deadline != 0 && now >= phase2_deadline)
            {
                ESP_LOGW(TAG, "connect: timeout GATT con '%s' (fase 2: %lu ms)",
                         dev_name, (unsigned long)eff_phase2_timeout_ms);
                if (conn_handle != BLE_HS_CONN_HANDLE_NONE && conn_handle != 0)
                {
                    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
                break;
            }

            if (!linked && now >= phase1_deadline)
            {
                ESP_LOGW(TAG, "connect: timeout de enlace con '%s' (fase 1: %lu ms); cancelando intento GAP",
                         dev_name, (unsigned long)eff_phase1_timeout_ms);
                int rc_cancel = ble_gap_conn_cancel();
                if (rc_cancel != 0 && rc_cancel != BLE_HS_EALREADY)
                {
                    ESP_LOGD(TAG, "ble_gap_conn_cancel rc=%d", rc_cancel);
                }
                break;
            }
        }

        if (conn_success && conn_handle != BLE_HS_CONN_HANDLE_NONE && conn_handle != 0)
        {
            *out_conn_handle = conn_handle;
            *out_char_handle = char_handle;
            ESP_LOGI(TAG, "connect: lista para '%s' (handle %u, char 0x%04X) en intento %d/%d",
                     dev_name, conn_handle, char_handle, attempt, (int)BLE_TRANSPORT_MAX_RETRIES);
            return ESP_OK;
        }

        /* Intento fallido: cancelar GAP y aplicar backoff antes de reintentar */
        ble_gap_conn_cancel();
        if (attempt < (int)BLE_TRANSPORT_MAX_RETRIES)
        {
            vTaskDelay(pdMS_TO_TICKS(BLE_TRANSPORT_RETRY_BACKOFF_MS));
        }
    }

    ESP_LOGE(TAG, "connect: conexion bajo demanda fallida tras %d intentos", (int)BLE_TRANSPORT_MAX_RETRIES);
    return ESP_FAIL;
}

/* ── Raw I/O ───────────────────────────────────────────────────────────── */

esp_err_t ble_transport_write_raw(uint16_t conn_handle, uint16_t char_handle,
                                  const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || conn_handle == 0 || char_handle == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, char_handle, data, len);
    if (rc == 0)
    {
        return ESP_OK;
    }
    if (rc == BLE_HS_ENOTCONN)
    {
        ESP_LOGE(TAG, "write_raw: link not connected (rc=7 BLE_HS_ENOTCONN). Aborting immediately.");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "write_raw: rc=%d (conn %u, char 0x%04X)", rc, conn_handle, char_handle);

    /* Auto-Recovery Rediscovery Fallback */
    if (rc == BLE_HS_EAPP || rc == BLE_HS_ENOENT || rc == BLE_HS_EINVAL || rc == 0x0101)
    {
        ESP_LOGW(TAG, "⚠️ Stale GATT handle (0x%04X) on conn %u. Invalidating cache and rediscovering...",
                 char_handle, conn_handle);
    }

    return ESP_FAIL;
}

/* ── Generic pulse-stop ────────────────────────────────────────────────── */

typedef struct
{
    uint16_t conn_handle;
    uint16_t char_handle;
    uint32_t delay_ms;
    uint8_t stop_data[BLE_TRANSPORT_STOP_DATA_MAX];
    uint16_t stop_len;
} ble_transport_pulse_stop_param_t;

static void ble_transport_pulse_stop_task(void *arg)
{
    ble_transport_pulse_stop_param_t *p = (ble_transport_pulse_stop_param_t *)arg;
    if (p != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(p->delay_ms));
        ESP_LOGI(TAG, "Impulso de %lu ms completado. Enviando STOP generico...",
                 (unsigned long)p->delay_ms);
        if (p->conn_handle != BLE_HS_CONN_HANDLE_NONE && p->conn_handle != 0 && p->stop_len > 0)
        {
            ble_transport_write_raw(p->conn_handle, p->char_handle, p->stop_data, p->stop_len);
            vTaskDelay(pdMS_TO_TICKS(300));
            ESP_LOGI(TAG, "Desconectando conexion BLE (handle %u) para liberar radio...",
                     p->conn_handle);
            ble_gap_terminate(p->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        free(p);
    }
    vTaskDelete(NULL);
}

esp_err_t ble_transport_pulse_stop(uint16_t conn_handle, uint16_t char_handle,
                                   uint32_t delay_ms,
                                   const uint8_t *stop_data, uint16_t stop_len)
{
    if (stop_data == NULL || stop_len == 0 || stop_len > BLE_TRANSPORT_STOP_DATA_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ble_transport_pulse_stop_param_t *param = heap_caps_malloc(
        sizeof(ble_transport_pulse_stop_param_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (param == NULL)
    {
        param = malloc(sizeof(ble_transport_pulse_stop_param_t)); /* Fallback a DRAM */
    }
    if (param == NULL)
    {
        ESP_LOGE(TAG, "pulse_stop: sin memoria para parametros");
        return ESP_ERR_NO_MEM;
    }

    param->conn_handle = conn_handle;
    param->char_handle = char_handle;
    param->delay_ms = delay_ms;
    param->stop_len = stop_len;
    memcpy(param->stop_data, stop_data, stop_len);

    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(ble_transport_pulse_stop_task,
                                                    "ble_pulse_gen",
                                                    BLE_TRANSPORT_PULSE_TASK_STACK,
                                                    param, 5, NULL, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS)
    {
        ESP_LOGE(TAG, "pulse_stop: no se pudo crear la tarea");
        free(param);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

