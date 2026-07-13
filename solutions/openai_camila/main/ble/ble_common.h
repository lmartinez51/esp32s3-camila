#ifndef BLE_COMMON_H
#define BLE_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLE_COMMON_INIT_TIMEOUT_MS 15000

typedef enum {
    BLE_COMMON_STATE_UNINITIALIZED = 0,
    BLE_COMMON_STATE_INITIALIZING,
    BLE_COMMON_STATE_READY,
    BLE_COMMON_STATE_PROVISIONING,
    BLE_COMMON_STATE_CENTRAL_DIAGNOSTIC,
    BLE_COMMON_STATE_STOPPING,
    BLE_COMMON_STATE_ERROR
} ble_common_state_t;

typedef enum {
    BLE_COMMON_ROLE_NONE = 0,
    BLE_COMMON_ROLE_PROVISIONING,
    BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC
} ble_common_role_t;

esp_err_t ble_common_init(SemaphoreHandle_t ext_sync_semaphore);
esp_err_t ble_common_ensure_ready(SemaphoreHandle_t ext_sync_semaphore,
                                  bool register_provisioning_services);
esp_err_t ble_common_deinit(uint32_t timeout_ms);

esp_err_t ble_common_acquire(ble_common_role_t role);
void ble_common_release(ble_common_role_t role);

bool ble_common_is_started(void);
bool ble_common_is_synced(void);
bool ble_common_services_registered(void);
ble_common_state_t ble_common_get_state(void);
ble_common_role_t ble_common_get_owner(void);

void nimble_host_task(void *param);

#ifdef __cplusplus
}
#endif

#endif
