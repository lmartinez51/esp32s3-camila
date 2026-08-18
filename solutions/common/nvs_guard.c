/**
 * @file nvs_guard.c
 * @brief Global recursive mutex serializing every NVS operation in the project.
 *
 * Moved here from config/nvs_setup.c so that common-component code
 * (network_storage.c) can take the same lock as solution code. Keeping a
 * single mutex (instead of one per component) is what makes NVS access
 * mutually exclusive across tasks running on both cores.
 *
 * @date 2026
 */

#include "nvs_guard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "NVS_GUARD"

static SemaphoreHandle_t nvs_mutex = NULL; // global mutex para acceso seguro a NVS

void nvs_setup_mutex_init(void)
{
    if (nvs_mutex == NULL)
    {
        nvs_mutex = xSemaphoreCreateRecursiveMutex();
        if (nvs_mutex == NULL)
        {
            ESP_LOGE(TAG, "Error creando mutex recursivo para NVS");
        }
        else
        {
            ESP_LOGI(TAG, "Mutex recursivo NVS inicializado correctamente");
        }
    }
}

void nvs_lock(void)
{
    if (nvs_mutex)
        xSemaphoreTakeRecursive(nvs_mutex, portMAX_DELAY);
}

void nvs_unlock(void)
{
    if (nvs_mutex)
        xSemaphoreGiveRecursive(nvs_mutex);
}