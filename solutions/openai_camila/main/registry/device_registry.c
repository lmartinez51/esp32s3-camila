/**
 * @file device_registry.c
 * @brief Robot registry bootstrap — drivers + NVS migration (Phase 2).
 *
 * The migration task runs on an INTERNAL SRAM stack (NVS flash access
 * disables the cache; PSRAM stacks would fault), pinned to core 0 at low
 * priority, mirroring the legacy nvs_save_worker_task pattern.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "device_registry.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "app_events.h"
#include "wifi_session_state.h"
#include "ble_elegoo_bt16.h"
#include "ble_generic_nus.h"
#include "ble_hue.h"
#include "wifi_tcp.h"
#include "wifi_http.h"
#include "wifi_arm.h"
#include "wifi_pantilt.h"
#include "ir_rmt.h"
#include "ir_nec.h"
#include "ir_sony.h"
#include "ir_rc5.h"
#include "registry_migrate.h"
#include "registry_persist.h"
#include "robot_hal.h"
#include "robot_health.h"

#define TAG "DEV_REGISTRY"

#define REGISTRY_MIGRATE_TASK_STACK 3072
#define REGISTRY_MIGRATE_WIFI_TIMEOUT_MS 30000

static TaskHandle_t s_registry_migrate_task_handle = NULL;

static void robot_registry_migrate_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        /* Clear-on-exit: re-runs whenever WiFi (re)connects, so profiles
         * provisioned after boot are picked up too. */
        EventBits_t bits = xEventGroupWaitBits(app_startup_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdTRUE,  /* xClearOnExit */
                                               pdFALSE, /* xWaitForAllBits */
                                               pdMS_TO_TICKS(REGISTRY_MIGRATE_WIFI_TIMEOUT_MS));
        if ((bits & WIFI_CONNECTED_BIT) == 0)
        {
            continue;
        }

        const char *ssid = wifi_session_get_connected_ssid();
        if (ssid == NULL || ssid[0] == '\0')
        {
            continue;
        }

        int migrated = 0;
        esp_err_t err = robot_registry_migrate_legacy_ble(ssid, &migrated);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Registry: %d dispositivo(s) migrado(s) para SSID '%s'",
                     migrated, ssid);

            /* One-shot: la migración terminó y esta tarea ya no tiene
             * trabajo. Se auto-elimina para liberar su stack interno
             * (3072 B) + TCB antes de STATE_IGNITING — el reaprovisionado
             * por reconexión vuelve a migrar en el próximo arranque. */
            s_registry_migrate_task_handle = NULL;
            ESP_LOGI(TAG, "Migración one-shot completada; liberando stack interno (%d B + TCB)",
                     REGISTRY_MIGRATE_TASK_STACK);
            vTaskDelete(NULL);
            return; /* inalcanzable */
        }
        else
        {
            ESP_LOGW(TAG, "Registry: migración falló para SSID '%s': %s",
                     ssid, esp_err_to_name(err));
        }
    }
}

esp_err_t robot_registry_init(void)
{
    esp_err_t err = robot_hal_init();
    if (err != ESP_OK)
    {
        return err;
    }

    err = robot_hal_register_driver(ble_elegoo_bt16_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver elegoo_bt16: %s", esp_err_to_name(err));
        return err;
    }

    /* Phase 3: driver genérico NUS / serial (auto-detectado en la migración). */
    err = robot_hal_register_driver(ble_generic_nus_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver ble_generic_nus: %s", esp_err_to_name(err));
        return err;
    }

    /* Driver de luz inteligente Philips Hue BLE. */
    err = robot_hal_register_driver(ble_hue_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver ble_hue: %s", esp_err_to_name(err));
        return err;
    }

    /* Phase 4: drivers WiFi (TCP raw / HTTP REST). */
    err = robot_hal_register_driver(wifi_tcp_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver wifi_tcp: %s", esp_err_to_name(err));
        return err;
    }

    err = robot_hal_register_driver(wifi_http_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver wifi_http: %s", esp_err_to_name(err));
        return err;
    }

    /* Phase 6: drivers de brazo y pan-tilt (servo controllers TCP). */
    err = robot_hal_register_driver(wifi_arm_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver wifi_arm: %s", esp_err_to_name(err));
        return err;
    }

    err = robot_hal_register_driver(wifi_pantilt_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver wifi_pantilt: %s", esp_err_to_name(err));
        return err;
    }

    /* Phase 5: motor RMT IR + codecs (NEC / Sony / RC5). */
    err = ir_rmt_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Fallo iniciando motor IR RMT: %s", esp_err_to_name(err));
        return err;
    }

    err = robot_hal_register_driver(ir_nec_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver ir_nec: %s", esp_err_to_name(err));
        return err;
    }

    err = robot_hal_register_driver(ir_sony_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver ir_sony: %s", esp_err_to_name(err));
        return err;
    }

    err = robot_hal_register_driver(ir_rc5_get_driver());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Fallo registrando driver ir_rc5: %s", esp_err_to_name(err));
        return err;
    }

    /* Phase 4: persistencia NVS del registry (worker core 0 + carga inicial). */
    err = registry_persist_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Fallo iniciando persistencia del registry: %s", esp_err_to_name(err));
        return err;
    }

    if (s_registry_migrate_task_handle == NULL)
    {
        if (xTaskCreatePinnedToCore(robot_registry_migrate_task,
                                    "reg_migrate",
                                    REGISTRY_MIGRATE_TASK_STACK,
                                    NULL,
                                    5,   /* low priority, like nvs_save worker */
                                    &s_registry_migrate_task_handle,
                                    0)   /* core 0: keep audio/WebRTC (core 1) free */
            != pdPASS)
        {
            ESP_LOGE(TAG, "No se pudo crear la tarea de migración del registry");
            s_registry_migrate_task_handle = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Tarea de migración NVS -> HAL registry creada (core 0, prio 5)");
    }

    /* Phase 7: monitor de salud (presupuesto de memoria + integridad). */
    err = robot_health_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Monitor de salud HAL no iniciado: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Robot registry inicializado (%d drivers multi-protocolo)", (int)robot_hal_get_driver_count());
    return ESP_OK;
}
