#include "ble_common.h"

#include "ble_config.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_COMMON";

static SemaphoreHandle_t sync_semaphore = NULL;
static SemaphoreHandle_t host_stop_semaphore = NULL;

static bool s_ble_common_configured = false;
static bool s_ble_host_start_requested = false;
static bool s_ble_host_started = false;
static bool s_ble_provisioning_services_registered = false;

static ble_common_state_t s_ble_state = BLE_COMMON_STATE_UNINITIALIZED;
static ble_common_role_t s_ble_owner = BLE_COMMON_ROLE_NONE;

static void ble_common_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host sincronizado");
    s_ble_host_started = true;

    if (s_ble_owner == BLE_COMMON_ROLE_NONE) {
        s_ble_state = BLE_COMMON_STATE_READY;
    }

    if (sync_semaphore != NULL) {
        xSemaphoreGive(sync_semaphore);
    }
}

static void ble_common_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset reason=%d", reason);
    s_ble_state = BLE_COMMON_STATE_ERROR;
    s_ble_owner = BLE_COMMON_ROLE_NONE;
}

void nimble_host_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "NimBLE host task iniciado");
    nimble_port_run();

    ESP_LOGW(TAG, "NimBLE host task detenido");
    s_ble_host_started = false;
    s_ble_host_start_requested = false;
    s_ble_common_configured = false;
    s_ble_provisioning_services_registered = false;
    s_ble_owner = BLE_COMMON_ROLE_NONE;
    s_ble_state = BLE_COMMON_STATE_UNINITIALIZED;

    if (host_stop_semaphore != NULL) {
        xSemaphoreGive(host_stop_semaphore);
    }

    nimble_port_freertos_deinit();
}

esp_err_t ble_common_init(SemaphoreHandle_t ext_sync_semaphore)
{
    if (ext_sync_semaphore == NULL) {
        ESP_LOGE(TAG, "BLE sync semaphore is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    sync_semaphore = ext_sync_semaphore;

    if (host_stop_semaphore == NULL) {
        host_stop_semaphore = xSemaphoreCreateBinary();
        if (host_stop_semaphore == NULL) {
            ESP_LOGE(TAG, "BLE host stop semaphore allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ble_common_configured) {
        return ESP_OK;
    }

    s_ble_state = BLE_COMMON_STATE_INITIALIZING;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        s_ble_state = BLE_COMMON_STATE_ERROR;
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    err = ble_wifi_register_services();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning service pre-registration failed: %s", esp_err_to_name(err));
        s_ble_state = BLE_COMMON_STATE_ERROR;
        return err;
    }
    s_ble_provisioning_services_registered = true;
    ESP_LOGI(TAG, "Provisioning GATT services pre-registered before host start");

    ble_hs_cfg.sync_cb = ble_common_on_sync;
    ble_hs_cfg.reset_cb = ble_common_on_reset;

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    s_ble_common_configured = true;
    s_ble_state = BLE_COMMON_STATE_READY;

    ESP_LOGI(TAG, "BLE common configured; bonds preserved");
    return ESP_OK;
}

esp_err_t ble_common_ensure_ready(SemaphoreHandle_t ext_sync_semaphore,
                                  bool register_provisioning_services)
{
    esp_err_t err = ble_common_init(ext_sync_semaphore);
    if (err != ESP_OK) {
        return err;
    }

    if (register_provisioning_services && !s_ble_provisioning_services_registered) {
        if (s_ble_host_start_requested || s_ble_host_started || ble_hs_synced()) {
            ESP_LOGE(TAG, "Cannot register provisioning services after NimBLE host start");
            return ESP_ERR_INVALID_STATE;
        }

        err = ble_wifi_register_services();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning service registration failed: %s", esp_err_to_name(err));
            s_ble_state = BLE_COMMON_STATE_ERROR;
            return err;
        }

        s_ble_provisioning_services_registered = true;
        ESP_LOGI(TAG, "Provisioning GATT services registered");
    }

    if (ble_hs_synced()) {
        s_ble_host_started = true;
        return ESP_OK;
    }

    if (!s_ble_host_start_requested) {
        xSemaphoreTake(sync_semaphore, 0);
        xSemaphoreTake(host_stop_semaphore, 0);
        s_ble_host_start_requested = true;
        s_ble_state = BLE_COMMON_STATE_INITIALIZING;
        nimble_port_freertos_init(nimble_host_task);
    }

    if (xSemaphoreTake(sync_semaphore, pdMS_TO_TICKS(BLE_COMMON_INIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for BLE host sync");
        s_ble_state = BLE_COMMON_STATE_ERROR;
        return ESP_ERR_TIMEOUT;
    }

    s_ble_host_started = true;
    s_ble_state = BLE_COMMON_STATE_READY;
    return ESP_OK;
}

esp_err_t ble_common_deinit(uint32_t timeout_ms)
{
    esp_err_t deinit_err = ESP_OK;
    bool wait_for_host_stop = false;

    if (!s_ble_common_configured &&
        !s_ble_host_start_requested &&
        !s_ble_host_started &&
        !ble_hs_synced())
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "BLE common deinit requested");
    s_ble_state = BLE_COMMON_STATE_STOPPING;
    s_ble_owner = BLE_COMMON_ROLE_NONE;

    if (host_stop_semaphore != NULL) {
        xSemaphoreTake(host_stop_semaphore, 0);
    }

    if (s_ble_host_start_requested || s_ble_host_started || ble_hs_synced()) {
        int stop_rc = nimble_port_stop();
        if (stop_rc != 0) {
            ESP_LOGW(TAG, "nimble_port_stop returned %d", stop_rc);
        } else {
            wait_for_host_stop = true;
        }
    }

    if (wait_for_host_stop && host_stop_semaphore != NULL) {
        if (xSemaphoreTake(host_stop_semaphore, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for NimBLE host task stop");
            s_ble_state = BLE_COMMON_STATE_ERROR;
            return ESP_ERR_TIMEOUT;
        }
    }

    deinit_err = nimble_port_deinit();
    if (deinit_err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(deinit_err));
        s_ble_state = BLE_COMMON_STATE_ERROR;
        return deinit_err;
    }

    s_ble_host_started = false;
    s_ble_host_start_requested = false;
    s_ble_common_configured = false;
    s_ble_provisioning_services_registered = false;
    s_ble_owner = BLE_COMMON_ROLE_NONE;
    s_ble_state = BLE_COMMON_STATE_UNINITIALIZED;

    if (host_stop_semaphore != NULL) {
        vSemaphoreDelete(host_stop_semaphore);
        host_stop_semaphore = NULL;
    }

    ESP_LOGI(TAG, "BLE common deinit complete");
    return ESP_OK;
}

esp_err_t ble_common_acquire(ble_common_role_t role)
{
    if (role == BLE_COMMON_ROLE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_common_is_synced()) {
        ESP_LOGE(TAG, "BLE role acquire rejected; host is not synced");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ble_owner == role) {
        return ESP_OK;
    }

    if (s_ble_owner != BLE_COMMON_ROLE_NONE) {
        ESP_LOGE(TAG, "BLE owner conflict current=%d requested=%d", s_ble_owner, role);
        return ESP_ERR_INVALID_STATE;
    }

    s_ble_owner = role;

    switch (role) {
    case BLE_COMMON_ROLE_PROVISIONING:
        s_ble_state = BLE_COMMON_STATE_PROVISIONING;
        break;
    case BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC:
        s_ble_state = BLE_COMMON_STATE_CENTRAL_DIAGNOSTIC;
        break;
    default:
        s_ble_owner = BLE_COMMON_ROLE_NONE;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "BLE owner acquired role=%d", role);
    return ESP_OK;
}

void ble_common_release(ble_common_role_t role)
{
    if (role == BLE_COMMON_ROLE_NONE) {
        return;
    }

    if (s_ble_owner != role) {
        return;
    }

    ESP_LOGI(TAG, "BLE owner released role=%d", role);
    s_ble_owner = BLE_COMMON_ROLE_NONE;
    s_ble_state = ble_common_is_synced() ? BLE_COMMON_STATE_READY
                                         : BLE_COMMON_STATE_UNINITIALIZED;
}

bool ble_common_is_started(void)
{
    return s_ble_host_started || ble_hs_synced();
}

bool ble_common_is_synced(void)
{
    return ble_hs_synced();
}

bool ble_common_services_registered(void)
{
    return s_ble_provisioning_services_registered;
}

ble_common_state_t ble_common_get_state(void)
{
    return s_ble_state;
}

ble_common_role_t ble_common_get_owner(void)
{
    return s_ble_owner;
}
