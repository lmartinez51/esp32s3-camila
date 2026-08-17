/**
 * @file registry_migrate.c
 * @brief Legacy NVS -> Robot HAL registry migration (Phase 2).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "registry_migrate.h"

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_setup.h"
#include "robot_hal.h"
#include "ble_generic_nus.h"
#include "ble_hue.h"
#include "registry_persist.h"

#define TAG "REG_MIGRATE"

static void ble_elegoo_format_uuid(char *out, size_t out_len, const uint8_t value[16])
{
    snprintf(out, out_len,
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             value[0], value[1], value[2], value[3], value[4], value[5],
             value[6], value[7], value[8], value[9], value[10], value[11],
             value[12], value[13], value[14], value[15]);
}

static bool ble_elegoo_contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL)
    {
        return false;
    }
    const size_t needle_len = strlen(needle);
    const size_t haystack_len = strlen(haystack);
    if (needle_len == 0 || needle_len > haystack_len)
    {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++)
    {
        if (strncasecmp(&haystack[i], needle, needle_len) == 0)
        {
            return true;
        }
    }
    return false;
}

static robot_category_t ble_elegoo_guess_category(const char *name, const char *alias)
{
    if (ble_elegoo_contains_ci(name, "elegoo") || ble_elegoo_contains_ci(alias, "elegoo") ||
        ble_elegoo_contains_ci(name, "carro") || ble_elegoo_contains_ci(alias, "carro"))
    {
        return ROBOT_CATEGORY_CAR;
    }
    if (ble_elegoo_contains_ci(name, "hue") || ble_elegoo_contains_ci(alias, "hue") ||
        ble_elegoo_contains_ci(name, "lamp") || ble_elegoo_contains_ci(alias, "lamp") ||
        ble_elegoo_contains_ci(name, "luz") || ble_elegoo_contains_ci(alias, "luz") ||
        ble_elegoo_contains_ci(name, "foco") || ble_elegoo_contains_ci(alias, "foco") ||
        ble_elegoo_contains_ci(name, "bulb") || ble_elegoo_contains_ci(alias, "bulb") ||
        ble_elegoo_contains_ci(name, "philips") || ble_elegoo_contains_ci(alias, "philips"))
    {
        return ROBOT_CATEGORY_LIGHT;
    }
    return ROBOT_CATEGORY_GENERIC;
}

esp_err_t robot_registry_migrate_legacy_ble(const char *ssid, int *migrated)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (migrated != NULL)
    {
        *migrated = 0;
    }

    device_profile_nvs_t *profiles = heap_caps_malloc(
        sizeof(device_profile_nvs_t) * ROBOT_REGISTRY_MAX_DEVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (profiles == NULL)
    {
        ESP_LOGE(TAG, "Sin memoria PSRAM para buffer de migración NVS");
        return ESP_ERR_NO_MEM;
    }

    nvs_setup_mutex_init();
    const int count = load_devices_for_ssid(ssid, profiles, ROBOT_REGISTRY_MAX_DEVICES);
    if (count <= 0)
    {
        ESP_LOGI(TAG, "No hay perfiles legados para SSID '%s'", ssid);
        heap_caps_free(profiles);
        return ESP_OK;
    }

    int registered = 0;
    for (int i = 0; i < count; i++)
    {
        const device_profile_nvs_t *p = &profiles[i];
        const char *alias = (p->alias[0] != '\0') ? p->alias : p->name;
        if (alias[0] == '\0')
        {
            ESP_LOGW(TAG, "Perfil %d sin nombre/alias, omitido", i);
            continue;
        }

        robot_device_t dev = {0};
        strlcpy(dev.alias, alias, sizeof(dev.alias));
        dev.protocol = ROBOT_PROTOCOL_BLE;
        dev.category = ble_elegoo_guess_category(p->name, p->alias);

        /* Auto-detección de perfil de driver (Phase 3+): NUS, Hue, o ELEGOO. */
        ble_elegoo_format_uuid(dev.endpoint.service_uuid, sizeof(dev.endpoint.service_uuid),
                               p->service_uuid.value);
        ble_elegoo_format_uuid(dev.endpoint.char_uuid, sizeof(dev.endpoint.char_uuid),
                               p->char_uuid.value);
        if (strcasecmp(dev.endpoint.service_uuid, BLE_GENERIC_NUS_SERVICE_UUID_STR) == 0)
        {
            strlcpy(dev.driver_profile_id, BLE_GENERIC_NUS_PROFILE_ID,
                    sizeof(dev.driver_profile_id));
        }
        else if (strcasecmp(dev.endpoint.service_uuid, BLE_HUE_SERVICE_UUID_STR) == 0 ||
                 dev.category == ROBOT_CATEGORY_LIGHT)
        {
            strlcpy(dev.driver_profile_id, BLE_HUE_PROFILE_ID,
                    sizeof(dev.driver_profile_id));
            dev.category = ROBOT_CATEGORY_LIGHT;
        }
        else
        {
            strlcpy(dev.driver_profile_id, "elegoo_bt16", sizeof(dev.driver_profile_id));
        }

        memcpy(dev.endpoint.addr, p->addr.val, sizeof(dev.endpoint.addr));
        dev.endpoint.addr_type = p->addr.type;
        snprintf(dev.endpoint.endpoint, sizeof(dev.endpoint.endpoint),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 p->addr.val[0], p->addr.val[1], p->addr.val[2],
                 p->addr.val[3], p->addr.val[4], p->addr.val[5]);

        dev.id = registry_device_id(dev.endpoint.endpoint);

        esp_err_t err = robot_hal_register_device(&dev);
        if (err == ESP_OK)
        {
            registered++;
        }
        else
        {
            ESP_LOGW(TAG, "No se pudo registrar '%s': %s", dev.alias, esp_err_to_name(err));
        }
    }

    heap_caps_free(profiles);

    ESP_LOGI(TAG, "Migración NVS completada: %d/%d dispositivos registrados (SSID '%s')",
             registered, count, ssid);

    if (migrated != NULL)
    {
        *migrated = registered;
    }
    return ESP_OK;
}
