/**
 * @file robot_health.c
 * @brief Robot HAL health monitor implementation (Phase 7).
 *
 * Periodically logs the memory budget (Internal + PSRAM, mirroring the
 * legacy ble_log_memory_snapshot format), the registry/driver snapshot,
 * per-driver in-flight counters, and runs a registry integrity pass:
 * every device must have a non-empty alias, a registered driver profile
 * and a parseable endpoint for its protocol. The crc32 blob checks on
 * load live in registry_persist/ir_rmt; this is the runtime consistency
 * pass over the live HAL registry.
 *
 * The task runs on a PSRAM stack at low priority; it only logs and never
 * blocks the audio pipeline.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "robot_health.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "robot_hal.h"
#include "robot_types.h"

#define TAG "ROBOT_HEALTH"

#if !defined(CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S)
#define CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S 60
#endif

#define ROBOT_HEALTH_TASK_STACK 4096
#define ROBOT_HEALTH_TASK_PRIO  1
#define ROBOT_HEALTH_TASK_CORE  1

static TaskHandle_t s_health_task_handle = NULL;

static void robot_health_log_memory_snapshot(void)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "[HAL_MEM] Internal: Free=%zu Largest=%zu Min=%zu | PSRAM: Free=%zu Largest=%zu Min=%zu",
             free_internal, largest_internal, min_internal,
             free_psram, largest_psram, min_psram);
}

static bool robot_health_driver_registered(const char *profile_id)
{
    if (profile_id == NULL || profile_id[0] == '\0')
    {
        return false;
    }
    const size_t n_drv = robot_hal_get_driver_count();
    for (size_t i = 0; i < n_drv; i++)
    {
        const robot_driver_t *drv = robot_hal_get_driver_at(i);
        if (drv != NULL && strcmp(drv->profile_id, profile_id) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool robot_health_endpoint_valid(const robot_device_t *dev)
{
    if (dev == NULL)
    {
        return false;
    }
    if (dev->endpoint.endpoint[0] == '\0')
    {
        return false;
    }
    switch (dev->protocol)
    {
        case ROBOT_PROTOCOL_BLE:
            return dev->endpoint.addr[0] != 0 || dev->endpoint.addr[1] != 0 ||
                   dev->endpoint.addr[2] != 0 || dev->endpoint.addr[3] != 0 ||
                   dev->endpoint.addr[4] != 0 || dev->endpoint.addr[5] != 0;
        case ROBOT_PROTOCOL_WIFI:
            return dev->endpoint.ip[0] != '\0' && dev->endpoint.port != 0;
        case ROBOT_PROTOCOL_IR:
            return dev->endpoint.gpio != 0;
        default:
            return false;
    }
}

static void robot_health_check_registry(void)
{
    const size_t n_dev = robot_hal_get_device_count();
    int anomalies = 0;

    for (size_t i = 0; i < n_dev; i++)
    {
        const robot_device_t *dev = robot_hal_get_device_at(i);
        if (dev == NULL)
        {
            anomalies++;
            ESP_LOGE(TAG, "Integridad: device[%d] no accesible", (int)i);
            continue;
        }
        if (dev->alias[0] == '\0')
        {
            anomalies++;
            ESP_LOGE(TAG, "Integridad: device[%d] sin alias", (int)i);
        }
        if (!robot_health_driver_registered(dev->driver_profile_id))
        {
            anomalies++;
            ESP_LOGE(TAG, "Integridad: device '%s' -> perfil '%s' no registrado",
                     dev->alias, dev->driver_profile_id);
        }
        if (!robot_health_endpoint_valid(dev))
        {
            anomalies++;
            ESP_LOGE(TAG, "Integridad: device '%s' -> endpoint invalido para protocolo %d",
                     dev->alias, (int)dev->protocol);
        }
    }

    const size_t n_drv = robot_hal_get_driver_count();
    for (size_t i = 0; i < n_drv; i++)
    {
        const robot_driver_t *drv = robot_hal_get_driver_at(i);
        if (drv == NULL || drv->profile_id[0] == '\0' || drv->execute == NULL)
        {
            anomalies++;
            ESP_LOGE(TAG, "Integridad: driver[%d] invalido", (int)i);
        }
    }

    if (anomalies > 0)
    {
        ESP_LOGW(TAG, "Integridad del registro: %d anomalia(s) detectada(s)", anomalies);
    }
}

static void robot_health_log_snapshot(void)
{
    const size_t n_dev = robot_hal_get_device_count();
    const size_t n_drv = robot_hal_get_driver_count();
    int present = 0;

    for (size_t i = 0; i < n_dev; i++)
    {
        const robot_device_t *dev = robot_hal_get_device_at(i);
        if (dev != NULL && dev->present)
        {
            present++;
        }
    }

    ESP_LOGI(TAG, "Registry: %d dispositivo(s) (%d presente(s)), %d driver(s)",
             (int)n_dev, present, (int)n_drv);

    for (size_t i = 0; i < n_drv; i++)
    {
        const robot_driver_t *drv = robot_hal_get_driver_at(i);
        if (drv == NULL)
        {
            continue;
        }
        const uint32_t inflight = robot_hal_get_driver_inflight_at(i);
        if (inflight > 0)
        {
            ESP_LOGW(TAG, "Driver '%s' con %u op(s) en vuelo", drv->profile_id, (unsigned)inflight);
        }
    }
}

static void robot_health_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Monitor de salud HAL iniciado (intervalo %d s)",
             (int)CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S * 1000));
        robot_health_log_memory_snapshot();
        robot_health_log_snapshot();
        robot_health_check_registry();
    }
}

esp_err_t robot_health_init(void)
{
    if (s_health_task_handle != NULL)
    {
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCoreWithCaps(robot_health_task, "robot_health",
                                        ROBOT_HEALTH_TASK_STACK, NULL,
                                        ROBOT_HEALTH_TASK_PRIO, &s_health_task_handle,
                                        ROBOT_HEALTH_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        ESP_LOGE(TAG, "No se pudo crear la tarea de salud HAL");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
