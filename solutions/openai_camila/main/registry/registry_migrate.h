/**
 * @file registry_migrate.h
 * @brief Legacy NVS -> Robot HAL registry migration (Phase 2).
 *
 * Reads the legacy device_profile_nvs_t blobs persisted by the old BLE
 * device control flow (nvs_setup.c) and registers each as a robot_device_t
 * in the HAL registry. Blobs are NOT deleted: the legacy module keeps
 * reading them, so migration is lossless and reversible.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef REGISTRY_MIGRATE_H
#define REGISTRY_MIGRATE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Migrate all legacy BLE device profiles of an SSID into the HAL registry.
 *
 * Profiles are matched to the elegoo_bt16 driver (the only live control
 * path today). Alias is preferred over the device name; when neither is
 * set the profile is skipped. Device id = CRC32(endpoint string).
 *
 * Safe to call from any task; NVS access is serialized by the internal
 * nvs_setup mutex. The profile buffer is allocated in PSRAM.
 *
 * @param ssid    WiFi SSID owning the profiles (non-empty).
 * @param migrated (optional) receives the number of devices registered.
 * @return ESP_OK (even if 0 profiles exist) or an error code.
 */
esp_err_t robot_registry_migrate_legacy_ble(const char *ssid, int *migrated);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_MIGRATE_H */
