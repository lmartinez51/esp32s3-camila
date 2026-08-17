/**
 * @file registry_persist.c
 * @brief Robot HAL registry NVS persistence (Phase 4).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "registry_persist.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_setup.h"
#include "robot_hal.h"

#define TAG "REG_PERSIST"

#define REGISTRY_NVS_NAMESPACE "robot_registry"
#define REGISTRY_SAVE_QUEUE_LEN 4
#define REGISTRY_WORKER_STACK   2048
#define REGISTRY_WORKER_PRIO    5
#define REGISTRY_WORKER_CORE    0
#define REGISTRY_WORKER_IDLE_MS 3000

typedef enum
{
    REGISTRY_OP_SAVE_DEVICE = 1,
    REGISTRY_OP_LOAD_ALL,
} registry_op_t;

typedef struct
{
    registry_op_t op;
    robot_device_persist_t dev;
} registry_queue_item_t;

static QueueHandle_t s_registry_queue = NULL;
static TaskHandle_t s_registry_worker = NULL;
static SemaphoreHandle_t s_registry_spawn_lock = NULL;

uint32_t registry_device_id(const char *endpoint)
{
    if (endpoint == NULL)
    {
        return 0;
    }
    return esp_rom_crc32_le(0, (const uint8_t *)endpoint, strlen(endpoint));
}

static uint32_t registry_persist_crc(const robot_device_persist_t *p)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p,
                            offsetof(robot_device_persist_t, crc32));
}

static void registry_persist_from_device(const robot_device_t *dev,
                                         robot_device_persist_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = ROBOT_REGISTRY_MAGIC;
    p->version = ROBOT_REGISTRY_VERSION;
    p->protocol = (uint8_t)dev->protocol;
    p->category = (uint8_t)dev->category;
    p->id = dev->id;
    strlcpy(p->alias, dev->alias, sizeof(p->alias));
    strlcpy(p->driver_profile_id, dev->driver_profile_id, sizeof(p->driver_profile_id));
    strlcpy(p->endpoint, dev->endpoint.endpoint, sizeof(p->endpoint));
    memcpy(p->addr, dev->endpoint.addr, sizeof(p->addr));
    p->addr_type = dev->endpoint.addr_type;
    strlcpy(p->ip, dev->endpoint.ip, sizeof(p->ip));
    p->port = dev->endpoint.port;
    p->gpio = dev->endpoint.gpio;
    p->value_handle = dev->endpoint.value_handle;
    p->notify_handle = dev->endpoint.notify_handle;
    p->cccd_handle = dev->endpoint.cccd_handle;
    strlcpy(p->service_uuid, dev->endpoint.service_uuid, sizeof(p->service_uuid));
    strlcpy(p->char_uuid, dev->endpoint.char_uuid, sizeof(p->char_uuid));
    p->crc32 = registry_persist_crc(p);
}

static void registry_persist_to_device(const robot_device_persist_t *p,
                                       robot_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->id = p->id;
    strlcpy(dev->alias, p->alias, sizeof(dev->alias));
    dev->protocol = (robot_protocol_t)p->protocol;
    dev->category = (robot_category_t)p->category;
    strlcpy(dev->driver_profile_id, p->driver_profile_id, sizeof(dev->driver_profile_id));
    strlcpy(dev->endpoint.endpoint, p->endpoint, sizeof(dev->endpoint.endpoint));
    memcpy(dev->endpoint.addr, p->addr, sizeof(dev->endpoint.addr));
    dev->endpoint.addr_type = p->addr_type;
    strlcpy(dev->endpoint.ip, p->ip, sizeof(dev->endpoint.ip));
    dev->endpoint.port = p->port;
    dev->endpoint.gpio = p->gpio;
    dev->endpoint.value_handle = p->value_handle;
    dev->endpoint.notify_handle = p->notify_handle;
    dev->endpoint.cccd_handle = p->cccd_handle;
    strlcpy(dev->endpoint.service_uuid, p->service_uuid, sizeof(dev->endpoint.service_uuid));
    strlcpy(dev->endpoint.char_uuid, p->char_uuid, sizeof(dev->endpoint.char_uuid));
}

static int registry_persist_count_devices(void)
{
    int count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NULL, REGISTRY_NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (res == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "dev_", 4) == 0)
        {
            count++;
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return count;
}

static esp_err_t registry_persist_save(const robot_device_persist_t *p)
{
    nvs_setup_mutex_init();
    nvs_lock();

    esp_err_t err = ESP_OK;
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(REGISTRY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (open_err != ESP_OK)
    {
        if (open_err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG, "nvs_open(%s) fallo: %s", REGISTRY_NVS_NAMESPACE,
                     esp_err_to_name(open_err));
        }
        nvs_unlock();
        return open_err;
    }

    char key[16];
    snprintf(key, sizeof(key), "dev_%08lx", (unsigned long)p->id);

    err = nvs_set_blob(handle, key, p, sizeof(*p));
    if (err == ESP_OK)
    {
        nvs_set_u32(handle, "magic", ROBOT_REGISTRY_MAGIC);
        nvs_set_u8(handle, "version", ROBOT_REGISTRY_VERSION);
        nvs_set_u8(handle, "device_count", (uint8_t)registry_persist_count_devices());
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    nvs_unlock();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Dispositivo '%s' persistido en NVS (key=%s)", p->alias, key);
    }
    else
    {
        ESP_LOGE(TAG, "Fallo persistiendo '%s': %s", p->alias, esp_err_to_name(err));
    }
    return err;
}

static void registry_persist_load(void)
{
    nvs_setup_mutex_init();
    nvs_lock();

    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(REGISTRY_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_err != ESP_OK)
    {
        if (open_err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG, "nvs_open(%s) fallo: %s", REGISTRY_NVS_NAMESPACE,
                     esp_err_to_name(open_err));
        }
        nvs_unlock();
        return;
    }

    int loaded = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NULL, REGISTRY_NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (res == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "dev_", 4) == 0)
        {
            robot_device_persist_t p = {0};
            size_t len = sizeof(p);
            const esp_err_t err = nvs_get_blob(handle, info.key, &p, &len);
            if (err == ESP_OK && len == sizeof(p) &&
                p.magic == ROBOT_REGISTRY_MAGIC &&
                p.version == ROBOT_REGISTRY_VERSION &&
                p.crc32 == registry_persist_crc(&p))
            {
                robot_device_t dev;
                registry_persist_to_device(&p, &dev);
                const esp_err_t rerr = robot_hal_register_device(&dev);
                if (rerr == ESP_OK)
                {
                    loaded++;
                }
                else
                {
                    ESP_LOGW(TAG, "Blob %s no registrable: %s", info.key,
                             esp_err_to_name(rerr));
                }
            }
            else
            {
                ESP_LOGW(TAG, "Blob %s corrupto o de otra version, omitido", info.key);
            }
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);
    nvs_unlock();

    ESP_LOGI(TAG, "Registry persistido: %d dispositivo(s) cargados de NVS", loaded);
}

static void registry_worker_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        registry_queue_item_t *item = NULL;
        if (xQueueReceive(s_registry_queue, &item, pdMS_TO_TICKS(REGISTRY_WORKER_IDLE_MS)) != pdTRUE)
        {
            /* Idle: sin trabajo pendiente. Auto-eliminarse libera el stack
             * interno (2048 B + TCB) para el resto del runtime; el proximo
             * enqueue recrea el worker bajo demanda (nvs_save_worker_task
             * duplicaria infraestructura y gastaria otro stack interno
             * permanente). La cola persiste: nada se pierde. */
            if (xSemaphoreTake(s_registry_spawn_lock, portMAX_DELAY) == pdTRUE)
            {
                if (uxQueueMessagesWaiting(s_registry_queue) == 0)
                {
                    s_registry_worker = NULL;
                    xSemaphoreGive(s_registry_spawn_lock);
                    ESP_LOGI(TAG, "Worker de persistencia en reposo: stack interno liberado (%d B + TCB)",
                             REGISTRY_WORKER_STACK);
                    vTaskDelete(NULL);
                    return; /* inalcanzable */
                }
                xSemaphoreGive(s_registry_spawn_lock);
            }
            continue;
        }

        if (item == NULL)
        {
            continue;
        }

        if (item->op == REGISTRY_OP_SAVE_DEVICE)
        {
            registry_persist_save(&item->dev);
        }
        else if (item->op == REGISTRY_OP_LOAD_ALL)
        {
            registry_persist_load();
        }

        heap_caps_free(item);
    }
}

static esp_err_t registry_persist_ensure_worker(void)
{
    if (s_registry_spawn_lock == NULL || s_registry_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_registry_spawn_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (s_registry_worker == NULL)
    {
        /* Guard: verificar si hay suficiente memoria SRAM interna contigua antes de intentar spawn */
        const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest_block < (REGISTRY_WORKER_STACK + 512))
        {
            ESP_LOGW(TAG, "Memoria interna contigua insuficiente (%u B < %u B) para worker de persistencia; operacion retenida en cola",
                     (unsigned)largest_block, (unsigned)(REGISTRY_WORKER_STACK + 512));
            xSemaphoreGive(s_registry_spawn_lock);
            return ESP_OK;
        }

        if (xTaskCreatePinnedToCore(registry_worker_task,
                                    "reg_persist",
                                    REGISTRY_WORKER_STACK,
                                    NULL,
                                    REGISTRY_WORKER_PRIO,
                                    &s_registry_worker,
                                    REGISTRY_WORKER_CORE) != pdPASS)
        {
            s_registry_worker = NULL;
            ESP_LOGW(TAG, "No se pudo recrear la tarea de persistencia reg_persist (memoria insuficiente)");
            err = ESP_ERR_NO_MEM;
        }
        else
        {
            ESP_LOGI(TAG, "Worker de persistencia recreado bajo demanda (core %d, prio %d, stack SRAM %d)",
                     REGISTRY_WORKER_CORE, REGISTRY_WORKER_PRIO, REGISTRY_WORKER_STACK);
        }
    }
    xSemaphoreGive(s_registry_spawn_lock);
    return err;
}

static esp_err_t registry_persist_enqueue(const registry_queue_item_t *item)
{
    if (s_registry_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    registry_queue_item_t *copy = heap_caps_malloc(sizeof(registry_queue_item_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para operacion de persistencia");
        return ESP_ERR_NO_MEM;
    }

    *copy = *item;
    if (xQueueSend(s_registry_queue, &copy, 0) != pdTRUE)
    {
        heap_caps_free(copy);
        ESP_LOGW(TAG, "Cola de persistencia llena, operacion omitida");
        return ESP_ERR_TIMEOUT;
    }

    /* Worker asegurado DESPUES del send: si se auto-elimino por inactividad,
     * se recrea y drena la cola (incluida la operacion recien encolada). El
     * send primero evita que el worker se destruya con trabajo pendiente. */
    return registry_persist_ensure_worker();
}

esp_err_t registry_persist_init(void)
{
    if (s_registry_queue != NULL)
    {
        return ESP_OK;
    }

    s_registry_spawn_lock = xSemaphoreCreateMutex();
    if (s_registry_spawn_lock == NULL)
    {
        ESP_LOGE(TAG, "No se pudo crear el lock de spawn del worker");
        return ESP_ERR_NO_MEM;
    }

    s_registry_queue = xQueueCreate(REGISTRY_SAVE_QUEUE_LEN, sizeof(registry_queue_item_t *));
    if (s_registry_queue == NULL)
    {
        vSemaphoreDelete(s_registry_spawn_lock);
        s_registry_spawn_lock = NULL;
        ESP_LOGE(TAG, "No se pudo crear la cola de persistencia");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = registry_persist_ensure_worker();
    if (err != ESP_OK)
    {
        vQueueDelete(s_registry_queue);
        s_registry_queue = NULL;
        vSemaphoreDelete(s_registry_spawn_lock);
        s_registry_spawn_lock = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Persistencia del registry iniciada (worker bajo demanda, stack SRAM %d)",
             REGISTRY_WORKER_STACK);

    /* Carga inicial asincrona: los dispositivos persistidos reentran al
     * registro RAM sin bloquear el arranque. El item se asigna en HEAP
     * (PSRAM) — nunca como local de pila: la tarea main tiene solo
     * 3584 B y registry_queue_item_t pesa ~224 B (misma clase de bug que
     * el desbordamiento original de ir_rmt_init). */
    registry_queue_item_t *load = heap_caps_malloc(sizeof(registry_queue_item_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (load != NULL)
    {
        memset(load, 0, sizeof(*load));
        load->op = REGISTRY_OP_LOAD_ALL;
        if (xQueueSend(s_registry_queue, &load, 0) != pdTRUE)
        {
            heap_caps_free(load);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para la carga inicial del registry");
    }

    return ESP_OK;
}

esp_err_t registry_save_device_async(const robot_device_t *dev)
{
    if (dev == NULL || dev->alias[0] == '\0' || dev->endpoint.endpoint[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    registry_queue_item_t item = {0};
    item.op = REGISTRY_OP_SAVE_DEVICE;
    registry_persist_from_device(dev, &item.dev);

    return registry_persist_enqueue(&item);
}

esp_err_t registry_persist_load_all(void)
{
    const registry_queue_item_t load = { .op = REGISTRY_OP_LOAD_ALL, .dev = {0} };
    return registry_persist_enqueue(&load);
}
