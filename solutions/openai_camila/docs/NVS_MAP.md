# NVS Map — openai_camila

Mapa autoritativo del almacenamiento NVS del proyecto. Todo acceso NVS debe
cumplir el contrato de concurrencia descrito abajo (mutex global + workers).

## Namespaces

| Namespace       | Contenido                          | Claves                                             | Tipo   | Escritor                          |
|-----------------|------------------------------------|----------------------------------------------------|--------|-----------------------------------|
| `ble_devices`   | Perfiles de dispositivos BLE por SSID | `D_<3B MAC LE>_<crc8 SSID>`                        | blob   | `nvs_setup.c` (sync, bajo lock)   |
| `ble_profiles`  | Base de perfiles conocidos         | `profiles_db`                                      | blob   | `nvs_setup.c` (sync, bajo lock)   |
| `settings`      | Configuración de la solución       | `openai_key`                                       | str    | `nvs_setup.c` (sync, bajo lock)   |
| `system`        | Flags de arranque / modo           | `prov_flag` (u8), `op_mode` (u8)                   | u8     | `nvs_setup.c` (sync, bajo lock)   |
| `robot_registry`| Dispositivos registrados (HAL)     | `dev_<crc32(endpoint)>`, `magic`, `version`, `device_count` | blob / u32 / u8 / u8 | `registry_persist.c` (worker core 0) |
| `ir_codes`      | Códigos IR aprendidos              | `lr_<crc32(id)>`                                   | blob   | `ir_rmt.c` (worker core 0)        |
| `wifi_config`   | Credenciales WiFi guardadas        | `wifi_<0..2>_ssid`, `wifi_<0..2>_password`         | str    | `network_storage.c` (sync, bajo lock) |

> Histórico: hasta la v1 los blobs `lr_*` vivían dentro de `robot_registry`.
> La migración one-shot (`ir_learn_nvs_migrate_legacy`) los mueve a
> `ir_codes` y limpia el namespace legado en el primer arranque del motor IR.

## Formatos de blob

| Clave                    | Estructura                  | Validación                                        |
|--------------------------|-----------------------------|---------------------------------------------------|
| `D_*`                    | `device_profile_nvs_t`      | tamaño exacto (`clean_invalid_ble_entries_from_nvs` borra los inválidos) |
| `profiles_db`            | `known_device_profile_t[]`  | tamaño derivado del conteo                        |
| `dev_<crc32>`            | `robot_device_persist_t`    | `magic == ROBOT_REGISTRY_MAGIC`, `version`, `crc32` |
| `lr_<crc32>`             | `ir_learn_blob_t`           | `magic == IR_LEARN_MAGIC`, `version`, `pulse_count <= 256`, `crc32` |

## Contrato de concurrencia (Fase 1)

- **Un único mutex global** (`nvs_guard.c`, componente `common`):
  `nvs_setup_mutex_init()` + `nvs_lock()` / `nvs_unlock()`. Recursivo: los
  helpers que toman el lock pueden anidarse.
- **Todo** acceso a NVS (open/get/set/erase/commit/iteradores) corre bajo
  `nvs_lock()`, **incluidos** los workers.
- **Nunca** flash I/O desde contextos de callback WebRTC/tool: registry e IR
  encolan en sus workers (core 0, stack SRAM interna, auto-eliminación por
  idle); wifi/BLE/settings usan helpers síncronos pero con lock.
- `erase_nvs()` (nvs_flash_erase + restart) toma el lock y lo libera antes
  del restart (quiescencia garantizada).

## Ciclo de vida de borrado (Fases 2–3)

| Operación                     | RAM registry (HAL)        | NVS registry           | IR codes        | BLE legacy       | WiFi        |
|-------------------------------|---------------------------|------------------------|-----------------|------------------|-------------|
| Tool `remove_device`          | `robot_hal_unregister_device` | `registry_delete_device_async` | `ir_learn_delete` (si IR) | `delete_device_profile_by_mac` (si BLE, cualquier SSID) | — |
| Tool `forget_wifi_network`    | —                         | —                      | —               | —                | `network_delete_wifi_credential_by_ssid` |
| Tool `erase_all_data`         | `robot_hal_clear_devices` | `registry_persist_erase_all` | `ir_learn_delete_all` | `nvs_wipe_ble_devices` + `nvs_wipe_ble_profiles` | `network_delete_all_wifi_credentials` |

Reglas:
- `registry_delete_device_async` / `registry_persist_erase_all` son
  idempotentes y asíncronos (worker). `device_count` se recalcula con el
  conteo real de blobs commiteados.
- `ir_learn_delete` / `ir_learn_delete_all` evictan la caché RAM y borran
  NVS en el worker; no-op si el motor IR nunca arrancó.
- `magic`/`version`/`device_count` se reescriben después del commit del
  blob (el iterador NVS no ve escrituras sin commit).

## Verificación manual

1. `erase_all_data` → el registry HAL queda vacío (`get_discovered_ble_devices`)
   y un reboot confirma NVS limpio: BLE se redescubre sin perfiles conocidos.
2. Registrar un dispositivo WiFi + aprender un código IR → reboot → el
   dispositivo reaparece (registry) y el IR se reenvía (caché recargada).
3. `remove_device` sobre el IR → el código aprendido no se reenvía tras
   reboot.
4. `forget_wifi_network` sobre la red activa → aplica al reconectar.
5. `remove_device` sobre un BLE guardado con otro SSID → el blob `D_*`
   correspondiente desaparece (borrado por MAC, sin depender del SSID).