/**
 * @file registry_persist.h
 * @brief Robot HAL registry NVS persistence (Phase 4).
 *
 * Namespace `robot_registry` (schema from the architecture plan, section 4):
 *   magic (u32) | version (u8) | device_count (u8) | dev_<crc32> (blob)
 *
 * WiFi/IP devices registered by voice (set_device_endpoint) survive
 * reboots. All NVS I/O runs on a dedicated worker task with an INTERNAL
 * SRAM stack (NVS access disables the cache) pinned to core 0, serialized
 * with the legacy nvs_setup mutex — writes are queued, never executed
 * from WebRTC/tool callback contexts.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef REGISTRY_PERSIST_H
#define REGISTRY_PERSIST_H

#include "esp_err.h"
#include "robot_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOT_REGISTRY_MAGIC    0x524D414Cu /* "RMAL" */
#define ROBOT_REGISTRY_VERSION  1u

/* Serialized form — fixed-width strings so blobs are layout-stable. */
typedef struct
{
    uint32_t magic;          /* ROBOT_REGISTRY_MAGIC          */
    uint8_t  version;        /* ROBOT_REGISTRY_VERSION        */
    uint8_t  protocol;       /* robot_protocol_t              */
    uint8_t  category;       /* robot_category_t              */
    uint8_t  reserved;
    uint32_t id;             /* CRC32(endpoint)               */
    char     alias[ROBOT_ALIAS_MAX_LEN];
    char     driver_profile_id[ROBOT_PROFILE_ID_MAX_LEN];
    char     endpoint[ROBOT_ENDPOINT_MAX_LEN];
    uint8_t  addr[6];        /* parsed BLE MAC                */
    uint8_t  addr_type;
    char     ip[16];         /* WiFi endpoint                 */
    uint16_t port;
    uint8_t  gpio;
    uint16_t value_handle;   /* cached GATT handles           */
    uint16_t notify_handle;
    uint16_t cccd_handle;
    char     service_uuid[36];
    char     char_uuid[36];
    uint32_t crc32;          /* checksum of all prior fields  */
} robot_device_persist_t;

/**
 * @brief Stable device id = CRC32(endpoint string) ("192.168.1.50:8000").
 */
uint32_t registry_device_id(const char *endpoint);

/**
 * @brief Initialize the persistence worker (queue + task, idempotent) and
 * enqueue a load-all so persisted devices re-enter the RAM registry.
 * @return ESP_OK on success.
 */
esp_err_t registry_persist_init(void);

/**
 * @brief Queue an async save of a device into NVS (worker context does
 * the flash I/O). Returns immediately; safe from any task context.
 * @return ESP_OK when queued, ESP_ERR_NO_MEM / ESP_ERR_TIMEOUT when the
 *         queue is full (save dropped, RAM registry still authoritative).
 */
esp_err_t registry_save_device_async(const robot_device_t *dev);

/**
 * @brief Queue a full reload from NVS into the RAM registry.
 * @return ESP_OK when queued.
 */
esp_err_t registry_persist_load_all(void);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_PERSIST_H */
