# Robot HAL Architecture Plan — Universal Multi-Device, Multi-Protocol Abstraction

**Target:** `solutions/openai_camila/`
**Author:** Architecture Review (openai_camila)
**Date:** 2026-08-14
**Status:** Phases 0–7 delivered (build clean); hardware gates pending (IR TV/AC, TCP arm/pantilt) + 72 h soak

---

## 1. Executive Summary

`openai_camila` currently treats "the robot" as a single hardcoded ELEGOO BT16 BLE car. BLE tool definitions live inside the WebRTC session-update JSON, tool dispatch is a hardcoded `strcmp` chain in `webrtc.c`, the GATT driver and the device registry are fused in a ~4,500-line `ble_device_control.c`, and the NVS schema (`device_profile_nvs_t`) is BLE-shaped.

This plan refactors toward a **Robot HAL**: a normalized command bus (`alias + action + params`), a **driver table** (function-pointer vtable), and a **protocol-agnostic persistent registry**. The ELEGOO BT16 driver is preserved verbatim as the first concrete driver. All critical system constraints (non-blocking audio, internal-SRAM NVS stacks, PSRAM bulk buffers, <500 ms fail-fast probes) are encoded as architectural rules, not afterthoughts.

---

## 2. Codebase Radiography & Decoupling Map

### 2.1 Coupling inventory

| # | Location | What is coupled | Coupling type |
|---|----------|-----------------|---------------|
| C1 | `main/webrtc/webrtc.c:1781-1828` | `control_ble_device` / `set_ble_device_alias` / `get_discovered_ble_devices` tool JSON hardcoded into the `session.update` tools array, with ELEGOO BT16 mentioned in the description text | **WebRTC ⇄ Device** (tool schema) |
| C2 | `main/webrtc/webrtc.c:2640-2758` | `ble_tool_handler_task` + `start_ble_tool_task`: hardcoded `strcmp` dispatch, calls `ble_device_get_summary_for_chatbot()`, `ble_device_send_command_by_alias_or_name()`, `ble_device_set_alias_by_name()` directly | **WebRTC ⇄ BLE driver** (control flow) |
| C3 | `main/webrtc/webrtc.c:2920-2927` | Special-case routing of the three BLE tool names in `process_json()` before the generic `classes[]` loop; also `webrtc.c:2813` `classes[]` list | **WebRTC ⇄ Tool routing** |
| C4 | `main/webrtc/webrtc.c:2678` | Default `dev_str = "ELEGOO BT16"` when model omits `device_name` | **WebRTC ⇄ Vendor name** (hardcoded default) |
| C5 | `main/ble/ble_device_control.c:119-121` | `ELEGOO_NOTIFY_VALUE_HANDLE 0x0003`, `ELEGOO_NOTIFY_CCCD_HANDLE 0x0004`, `ELEGOO_COMMAND_VALUE_HANDLE 0x0006` | **Driver ⇄ Vendor GATT map** |
| C6 | `main/ble/ble_device_control.c:2642-2698` | Profile auto-match heuristics: `strstr(name,"ELEGOO")`, alias `"Carro"`, `matched_profile_index == 999` | **Driver ⇄ Vendor naming** |
| C7 | `main/ble/ble_device_control.c` (cmd table + `ble_find_command_entry()` + `send_elegoo_command_payload()` at 4186-4215) | Action→payload mapping `{"N":2,"D1":5}` (the actual ELEGOO protocol encoder) fused with the NimBLE central transport | **Protocol encoder ⇄ Transport** |
| C8 | `main/ble/ble_device_control.c:4235+` | On-demand connect + phased wait loop (8 s link / 6 s GATT, abort on early failure) fused with command execution | **Transport policy ⇄ Command execution** |
| C9 | `main/ble/ble_device_control.c:3987` (`ble_device_get_summary_for_chatbot`) | LLM-facing JSON catalog format owned by the driver | **Driver ⇄ LLM contract** |
| C10 | `main/config/nvs_setup.c:30-40` | `device_profile_nvs_t` is BLE-shaped (`ble_addr_t`, `service_uuid`, `char_uuid`); no protocol field, no endpoint abstraction | **Registry schema ⇄ BLE** |
| C11 | `main/config/nvs_setup.c:292-368` | Async NVS worker `nvs_save_worker_task` (internal SRAM stack, low priority, core 0) — **already correct**, must be preserved/reused | Good pattern (keep) |
| C12 | `main/webrtc/webrtc.c:701-717` | `create_psram_task()` — PSRAM stack helper for heavy tool tasks — **already correct** | Good pattern (keep) |

### 2.2 What to keep vs. extract

- **Keep untouched (owned by NimBLE layer):** `ble_common.c`, `ble_config.c`, `ble_device_callbacks.c` — GAP/GATT event plumbing.
- **Extract into `drivers/ble/ble_elegoo_bt16.c`:** items C5, C6, C7, and the command part of C8 (phased wait loop stays as shared *BLE transport policy*, parametrized by profile).
- **Extract into `adapters/webrtc_tool_adapter.c`:** C1, C2, C3, C4 (tool JSON becomes data, dispatch becomes table-driven).
- **Extract into `registry/`:** C9 (summary JSON becomes a registry concern, category-aware), C10 (new `robot_device_t` schema; old namespace kept for migration), reusing C11 worker pattern.
- **WebRTC/audio pipeline (I2S, `pc_task` 4 KB stack, `webrtc_action_task`):** untouched. HAL commands are enqueued; WebRTC never blocks on hardware.

---

## 3. HAL Architecture & Directory Structure

### 3.1 Layout

```
main/
├── hal/                          # Core abstractions (no vendor logic)
│   ├── robot_types.h             # Enums + value structs (actions, params, result)
│   ├── robot_hal.h               # Public facade API (init/execute/probe/register)
│   ├── robot_hal.c               # Facade + command bus + driver table
│   ├── robot_tools.h             # Tool-catalog builder (JSON for session.update)
│   └── robot_tools.c             # Builds tool JSON from registry categories
├── drivers/                      # Concrete transports + protocol codecs
│   ├── driver_api.h              # robot_driver_t vtable (may live in robot_hal.h)
│   ├── ble/
│   │   ├── ble_transport.c/h     # NimBLE central shared infra (scan, connect, GATT disc,
│   │   │                         #   phased-wait policy) — carved from ble_device_control.c
│   │   ├── ble_elegoo_bt16.c/h   # ELEGOO BT16 protocol encoder (cmd table + payload JSON)
│   │   └── ble_generic.c/h       # NUS / proprietary serial / generic GATT write driver
│   ├── wifi/
│   │   ├── wifi_tcp.c/h          # Raw TCP/UDP socket driver
│   │   ├── wifi_http.c/h         # HTTP REST driver (esp_http_client)
│   │   └── wifi_ws.c/h           # WebSocket driver (Phase 5)
│   └── ir/
│       ├── ir_rmt.c/h            # ESP-IDF RMT 38 kHz TX/RX + RAW pulse engine
│       ├── ir_nec.c/h            # NEC codec
│       ├── ir_sony.c/h           # Sony SIRC codec
│       └── ir_rc5.c/h            # RC5 codec
├── adapters/
│   ├── webrtc_tool_adapter.c/h   # process_json tool routing (replaces C2/C3 hardcoded chain)
│   └── voice_result.c/h          # robot_result_t → LLM JSON (success/error/telemetry)
└── registry/
    ├── device_registry.c/h       # RAM registry: alias lookup, probe cache, summaries
    ├── registry_nvs.c/h          # Persistent store (v2 schema) + async internal-stack worker
    └── registry_migrate.c/h      # One-shot migration from legacy BLE NVS namespace
```

**Back-compat shim (zero-regression):** `main/ble/ble_device_control.c` is *not deleted* in early phases. It remains the transport core. `ble_elegoo_bt16` initially wraps its public entry points; the old symbols are removed only after the HAL path is battle-tested (Phase 3+).

### 3.2 Core data structures (`hal/robot_types.h`)

```c
typedef enum {
    ROBOT_PROTOCOL_NONE = 0,
    ROBOT_PROTOCOL_BLE,
    ROBOT_PROTOCOL_WIFI,
    ROBOT_PROTOCOL_IR
} robot_protocol_t;

typedef enum {
    ROBOT_CATEGORY_CAR = 0,
    ROBOT_CATEGORY_ARM,
    ROBOT_CATEGORY_PAN_TILT,
    ROBOT_CATEGORY_IR_ACTUATOR,
    ROBOT_CATEGORY_GENERIC
} robot_category_t;

/* Normalized action IDs (superset covering all four device classes) */
typedef enum {
    ROBOT_ACTION_NONE = 0,
    /* Car */
    ROBOT_ACTION_FORWARD,  ROBOT_ACTION_BACKWARD,
    ROBOT_ACTION_LEFT,     ROBOT_ACTION_RIGHT,
    ROBOT_ACTION_STOP,     ROBOT_ACTION_ROTATE,
    ROBOT_ACTION_READ_ULTRASONIC, ROBOT_ACTION_READ_LINE_SENSOR, ROBOT_ACTION_READ_BATTERY,
    /* Arm */
    ROBOT_ACTION_GRAB,     ROBOT_ACTION_RELEASE,
    ROBOT_ACTION_ARM_UP,   ROBOT_ACTION_ARM_DOWN,
    ROBOT_ACTION_ARM_HOME, ROBOT_ACTION_MOVE_AXIS,
    /* Pan-Tilt */
    ROBOT_ACTION_PAN_LEFT, ROBOT_ACTION_PAN_RIGHT,
    ROBOT_ACTION_TILT_UP,  ROBOT_ACTION_TILT_DOWN,
    ROBOT_ACTION_CENTER,
    /* IR */
    ROBOT_ACTION_SEND_IR_COMMAND, ROBOT_ACTION_LEARN_IR_CODE
} robot_action_id_t;

typedef struct {
    uint32_t    speed;        /* 0..100 (%)                     */
    uint32_t    duration_ms;  /* pulse duration / stop-delay    */
    int32_t     angle_deg;    /* 0..180 for arm/pan-tilt        */
    uint8_t     axis_id;      /* 0=base,1=shoulder,2=elbow,3=gripper */
    uint8_t     ir_protocol;  /* NEC | SONY | RC5 | RAW         */
    uint32_t    ir_address;
    uint32_t    ir_command;
    const uint32_t *raw_timings; /* RAW pulse train (PSRAM alloc) */
    uint16_t    raw_len;
} robot_action_params_t;

typedef enum {
    ROBOT_RESULT_OK = 0,
    ROBOT_RESULT_ERR_NOT_FOUND,   /* alias unknown        */
    ROBOT_RESULT_ERR_OFFLINE,     /* probe failed fast    */
    ROBOT_RESULT_ERR_TIMEOUT,     /* driver timeout       */
    ROBOT_RESULT_ERR_TRANSPORT,   /* connection/write fail*/
    ROBOT_RESULT_ERR_UNSUPPORTED, /* action not in driver capabilities */
    ROBOT_RESULT_ERR_INVALID_ARG
} robot_result_code_t;

typedef struct {
    robot_result_code_t code;
    char detail[128];       /* human-readable, e.g. "Robot apagado"   */
    char telemetry[128];    /* e.g. "25 cm" (ultrasonic)              */
} robot_result_t;
```

### 3.3 Driver vtable (`hal/robot_hal.h`)

```c
typedef struct robot_driver robot_driver_t;

typedef esp_err_t (*robot_driver_init_fn)(robot_driver_t *drv);
typedef esp_err_t (*robot_driver_execute_fn)(robot_driver_t *drv,
                                             const char *alias,
                                             robot_action_id_t action,
                                             const robot_action_params_t *params,
                                             robot_result_t *out);
/* Fail-fast connectivity probe: MUST return in < ROBOT_PROBE_TIMEOUT_MS (500). */
typedef esp_err_t (*robot_driver_probe_fn)(robot_driver_t *drv,
                                           const robot_endpoint_t *ep,
                                           bool *present);
typedef void      (*robot_driver_deinit_fn)(robot_driver_t *drv);

typedef struct {
    char    profile_id[16];        /* "elegoo_bt16" | "ir_nec" | "wifi_http_rest" ... */
    robot_category_t   category;
    robot_protocol_t   protocol;
    uint32_t capabilities;         /* bitmask of robot_action_id_t this driver supports */
    robot_driver_init_fn    init;
    robot_driver_execute_fn execute;
    robot_driver_probe_fn   probe;
    robot_driver_deinit_fn  deinit;
    void   *priv;                  /* driver instance context, MALLOC_CAP_SPIRAM */
} robot_driver_t;

typedef struct {
    char endpoint[48];   /* "AA:BB:CC:DD:EE:FF" | "192.168.1.50:8000" | "gpio:17" */
    uint8_t addr[6];     /* parsed BLE MAC                                 */
    uint8_t addr_type;
    char ip[16];
    uint16_t port;
    uint8_t gpio;
    char service_uuid[36];  /* BLE */
    char char_uuid[36];     /* BLE */
    uint16_t value_handle;  /* BLE (cached after discovery) */
    uint16_t notify_handle; /* BLE */
    uint16_t cccd_handle;   /* BLE */
} robot_endpoint_t;

typedef struct {
    uint32_t         id;             /* stable id = CRC32(endpoint)        */
    char             alias[32];      /* "Carro", "Brazo", "Camara"        */
    robot_protocol_t protocol;
    robot_category_t category;
    char             driver_profile_id[16];
    robot_endpoint_t endpoint;
    /* RAM-only runtime state */
    bool             present;
    uint32_t         last_seen_ms;
} robot_device_t;
```

### 3.4 Command flow (normalized bus)

```
WebRTC tool call ("control_robot", alias="Carro", action="FORWARD", speed=70)
   │
   ▼
adapters/webrtc_tool_adapter.c     (PSRAM task, same pattern as ble_tool_handler_task)
   │  robot_hal_execute(alias, action, &params, callback_ctx)
   ▼
hal/robot_hal.c                    (registry lock held briefly; NO blocking I/O here)
   ├─ registry lookup  alias → robot_device_t
   │     (miss → ROBOT_RESULT_ERR_NOT_FOUND, answer in ~1 ms)
   ├─ driver table lookup  profile_id → robot_driver_t
   ├─ (optional) probe: driver->probe(&ep)  — bounded ≤ 500 ms
   └─ enqueue normalized command on driver's work queue  (return immediately)
   ▼
drivers/<protocol>/... worker task (PSRAM stack, driver-owned)
   ├─ transport operation (BLE connect/write, TCP send, RMT TX)
   ├─ robot_result_t → callback → adapters/voice_result.c → send_function_output(call_id)
   └─ telemetry actions fill out->telemetry
```

**Non-blocking rule (system constraint #1):** `robot_hal_execute()` is **enqueue-and-return**. Every driver owns its worker task (pattern: existing `ble_control_worker_task`, `ble_device_control.c:425`). The only bounded synchronous call allowed is `probe()`, hard-capped at `CONFIG_ROBOT_PROBE_TIMEOUT_MS` (default **400 ms**) so the LLM tool round-trip keeps the voice flow responsive and the I2S pipeline is never starved.

---

## 4. NVS Device Registry Schema

### 4.1 New namespace `robot_registry` (protocol-agnostic)

| Key | Type | Content |
|-----|------|---------|
| `magic` | u32 | `0x524D414C` ("RMAL") — schema fingerprint |
| `version` | u8 | 1 |
| `device_count` | u8 | 0..25 |
| `dev_<crc32(endpoint)>` | blob | Serialized `robot_device_persist_t` (below) |

```c
#define ROBOT_REGISTRY_MAGIC 0x524D414Cu
#define ROBOT_REGISTRY_VERSION 1u
#define ROBOT_REGISTRY_MAX_DEVICES 25u

/* Serialized form — fixed-width strings so blobs are layout-stable across builds */
typedef struct {
    uint32_t magic;          /* ROBOT_REGISTRY_MAGIC          */
    uint8_t  version;        /* ROBOT_REGISTRY_VERSION        */
    uint8_t  protocol;       /* robot_protocol_t              */
    uint8_t  category;       /* robot_category_t              */
    uint8_t  reserved;
    uint32_t id;             /* CRC32(endpoint)               */
    char     alias[32];      /* "Carro"                       */
    char     driver_profile_id[16]; /* "elegoo_bt16"          */
    char     endpoint[48];   /* MAC / IP:port / gpio:N        */
    uint8_t  addr[6];        /* parsed BLE MAC                */
    uint8_t  addr_type;
    char     ip[16];
    uint16_t port;
    uint8_t  gpio;
    uint16_t value_handle;   /* cached GATT handles (perf)    */
    uint16_t notify_handle;
    uint16_t cccd_handle;
    char     service_uuid[36];
    char     char_uuid[36];
    uint32_t crc32;          /* checksum of all prior fields  */
} robot_device_persist_t;
```

### 4.2 Access rules (system constraints #2, #4)

- **All flash I/O** runs on the existing single-writer pattern: a **registry NVS worker task with an INTERNAL SRAM stack** (clone of `nvs_save_worker_task`, `nvs_setup.c:292`), priority 5, core 0, guarded by the existing `nvs_lock()`/`nvs_unlock()` mutex.
- **Writes are queued** (`registry_save_device_async()`), never executed from WebRTC/NimBLE callback contexts.
- **RAM cache** (`robot_device_t` array, `MALLOC_CAP_SPIRAM`) is the read path; NVS is read once at boot + on explicit reload.

### 4.3 Migration & backward compatibility (constraint #5)

- Legacy `device_profile_nvs_t` (namespace `ble_devices`, keys by `ssid+maccrc`) is **left read-only**.
- `registry_migrate.c` runs one-shot at boot: legacy → `robot_registry` (protocol=BLE, profile=`elegoo_bt16`, alias preserved). Old namespace is never written again; removal is a later cleanup milestone.
- If the legacy path is needed for fallback, `ble_device_control.c` keeps reading the old namespace until Phase 4.

---

## 5. Phased, Non-Breaking Implementation Roadmap

Every phase ends in a flashable, voice-verifiable build. The ELEGOO car keeps working from day 1.

### Phase 0 — Baseline & gates (1 build)
- Snapshot current behavior: BLE summary JSON shape, alias/NVS files, `free internal/PSRAM` at boot and during audio.
- Add `sdkconfig.defaults`: `CONFIG_ROBOT_HAL=y`, `CONFIG_ROBOT_PROBE_TIMEOUT_MS=400`.
- **Gate:** current firmware builds and voice-controls the car; audio never drops.

### Phase 1 — Skeleton + WebRTC decoupling (pure refactor, no behavior change)
- Create `hal/robot_types.h`, `robot_hal.h`, empty registry.
- Move C1 (tool JSON) into `robot_tools.c` builders keyed by registered categories; JSON output byte-identical to today.
- Move C2/C3 into `adapters/webrtc_tool_adapter.c` with a **tool dispatch table** (`{name, handler_fn}`) registered from HAL; the handlers *still call the existing `ble_device_*` functions* (compat shim).
- **Gate:** identical summary JSON; all three BLE tools behave as before; zero changes to `ble/`.

### Phase 2 — First driver extraction: ELEGOO BT16 preserved
- `drivers/ble/ble_elegoo_bt16.c`: adopt cmd table + payload encoder (C7) and GATT handle knowledge (C5/C6) as `priv` data; `execute()` wraps `ble_device_send_command_by_alias_or_name`-equivalent path via `ble_transport`.
- `registry/device_registry.c`: load legacy aliases via `registry_migrate.c`; `robot_hal_execute("Carro","FORWARD",{speed,duration})` is now the live path; `get_discovered_ble_devices` summary becomes registry-driven (C9).
- **Gate:** car movement + `READ_ULTRASONIC` telemetry identical; NVS blobs migrate without loss.

### Phase 3 — Generic BLE profiles + fast-fail probes  ✅ delivered (build clean, 0x2dc990 / 28% free)
- `ble_transport.c`: probe = standalone **bounded passive scan** (MAC match, `CONFIG_ROBOT_PROBE_TIMEOUT_MS`=400 ms, early-exit + cancel+drain; EBUSY → degrade "assume present"); connect policy = phased wait (8 s / 6 s) riding the legacy connection owner via `ble_device_connect()` + reader over `ble_device_get_discovered_list` snapshots (PSRAM-buffered); raw write via `ble_gattc_write_no_rsp_flat`; `pulse_stop` = STOP payload + `ble_gap_terminate` from a small task.
- `ble_generic_nus.c`: NUS (`6E400001-...`) driver with v1 payload table F/B/L/R/S (per-device overrides deferred to Phase 4); `execute` = registry lookup → connect-when-waiting → raw write → pulse-stop for timed motion; auto-detected in `registry_migrate.c` from the service UUID.
- `probe()` live for both BLE drivers before any connect attempt → offline answer to LLM in <500 ms (`ROBOT_RESULT_ERR_OFFLINE`, no legacy fallback).
- `main/Kconfig`: `CONFIG_ROBOT_HAL=y`, `CONFIG_ROBOT_PROBE_TIMEOUT_MS=400`, `CONFIG_ROBOT_PROBE_CACHE_MS=10000` (absent never cached; present re-probed after 10 s).
- **Phase 3-b (deferred, not in gate):** deep carve-out of connect/GATT-discovery from the legacy `ble_device_control.c` module (NimBLE allows a single central consumer; the legacy module keeps `BLE_COMMON_ROLE_CENTRAL_DIAGNOSTIC` during WebRTC sessions) + full ELEGOO cutover to `ble_transport`-owned connections.
- **Gate:** ELEGOO + one NUS device (e.g. HM-10/AT09) both voice-controllable.

### Phase 4 — WiFi transports  ✅ delivered (build clean, 0x2dee00 / 28% free)
- `drivers/wifi/wifi_tcp.c`: raw TCP driver; probe = non-blocking TCP connect polled with `select()` (≤ `CONFIG_ROBOT_PROBE_TIMEOUT_MS`=400 ms, WiFi-down/refused/timeout → fail-fast offline); execute = bounded connect+send (`CONFIG_ROBOT_WIFI_EXEC_TIMEOUT_MS`=1500 ms) with pulse-stop task (PSRAM stack) for timed motion. v1 payload table F/B/L/R/S (ASCII car kits).
- `drivers/wifi/wifi_http.c`: HTTP REST driver over `esp_http_client`; probe reuses the shared `wifi_tcp_probe_tcp()` TCP-connect helper; execute POSTs JSON `{"action","speed","duration_ms"}` to `http://<ip>:<port><CONFIG_ROBOT_HTTP_COMMAND_PATH>` (default `/command`).
- `registry/registry_persist.c`: NVS namespace `robot_registry` (magic `RMAL` v1, blob per device `dev_<crc32(endpoint)>`, crc32 checksum) with a dedicated worker task (INTERNAL SRAM stack, prio 5, core 0, `nvs_lock()` serialized); `registry_save_device_async()` + boot load-all. IP endpoints (`alias ↔ ip:port`) ride the existing `robot_endpoint_t.ip/port` fields; `robot_hal_execute()` routes them with no changes (registry/driver lookup is protocol-agnostic).
- Tool catalog: new `set_device_endpoint` voice tool (alias, ip, port, protocol tcp|http, category) → adapter handler registers + persists; `control_ble_device` description updated to cover WiFi targets (rename to `control_robot` stays in Phase 6).
- **Gate (hardware):** first TCP-controlled robot (e.g. an ESP32 HTTP car) answers `FORWARD`/`STOP`. Not yet exercised on live hardware; compile-verified with zero BLE-path regressions.

### Phase 5 — IR (RMT) drivers  ✅ delivered (build clean, 0x2e6ff0 / 27% free)
- `drivers/ir/ir_rmt.c/h`: shared RMT engine. TX channel 1 MHz / 192 symbols / copy encoder; carrier applied per-send via `rmt_apply_carrier` (NEC/RC5 38/36 kHz, SONY 40 kHz; driver fixed 50% duty in v5.4); non-blocking TX from a PSRAM-stack task (`ir_tx`, stack 3072, prio 5, core 1, tx mutex). RX channel (1 MHz, 192 symbols) with `signal_range_min_ns`=200 µs glitch filter and `signal_range_max_ns`=50 ms idle → frame end (`on_recv_done` semaphore, PSRAM buffer). `probe()` = virtual presence (IR has no link). Shared `ir_rmt_codec_execute()` routes `SEND_IR_COMMAND` (RAW timings → codec encode → replay learned) and `LEARN_IR_CODE` (bounded capture ≤ `CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS`=2500 ms, decode order: device codec → NEC → SONY → RC5, always saves RAW).
- `drivers/ir/ir_nec.c/h`, `ir_sony.c/h`, `ir_rc5.c/h`: codecs (`ir_codec_t` descriptor + `const ir_codec_t ir_<proto>_codec` + driver getter). NEC 38 kHz (header 9/4.5 ms, 32-bit LSB addr+~addr+cmd+~cmd, bit 562/562 vs 562/1687, stop); SONY 40 kHz (12-bit SIRC, header 2.4/0.6 ms, 7 addr + 5 cmd LSB, bit 600/600 vs 600/1200, stop); RC5 36 kHz (14 bits, 889 µs halves, value = second half, 2 start bits, 5 addr + 6 cmd MSB-first, leading idle half omitted). Decoders validate with ±30 % tolerance and inversion checks.
- Learned codes: RAM cache (PSRAM, 8 slots, per-id) + NVS `robot_registry` blobs `lr_<crc32(endpoint)>` (magic `ILMR` v1, crc32, ~2 KB < 4 KB NVS blob limit) written by a dedicated worker (`ir_learn`, INTERNAL SRAM stack 4096, prio 5, core 0, `nvs_lock()` serialized); boot load-all. `SEND_IR_COMMAND` with `address=0, command=0` replays the learned code.
- Registry/drivers: 3 IR drivers registered in `device_registry.c` (protocol `ROBOT_PROTOCOL_IR`, category `IR_ACTUATOR`, caps `SEND_IR_COMMAND | LEARN_IR_CODE`) + `ir_rmt_init()` at boot. `Kconfig`/`sdkconfig.defaults`: `CONFIG_ROBOT_IR_TX_GPIO`=17, `CONFIG_ROBOT_IR_RX_GPIO`=18 (hardware-dependent, per-board menuconfig), `CONFIG_ROBOT_IR_LEARN_TIMEOUT_MS`=2500.
- Tool catalog: `control_ble_device` extended with `SEND_IR_COMMAND`/`LEARN_IR_CODE` + `ir_protocol`/`ir_address`/`ir_command`; new `set_ir_device` tool (alias + protocol NEC|SONY|RC5) → adapter handler registers + persists (`endpoint "gpio:<tx>"`). IR actions never fall back to the legacy BLE path — `hal_res.detail` is the final answer (learn/send telemetry in the JSON response).
- **Gate (hardware):** IR TV/AC relayed via voice; learned codes persist across reboot. Pins 17/18 are defaults only — verify against the board schematic.

### Phase 6 — Arm & Pan-Tilt + dynamic tool catalog  ✅ delivered (build clean, 0x2e7ab0 / 27% free)
- `drivers/wifi/wifi_arm.c/h`: robotic-arm driver over the shared bounded TCP transport (`wifi_tcp_probe_tcp` + new `wifi_tcp_send_endpoint()` helper). Profile `wifi_arm`, category `ARM`, capabilities GRAB/RELEASE/ARM_UP/ARM_DOWN/ARM_HOME/MOVE_AXIS. v1 ASCII payloads `GRAB`/`RELEASE`/`UP`/`DOWN`/`HOME` + parameterized `AXIS:<id>:<angle>` (params `axis_id`/`angle_deg`).
- `drivers/wifi/wifi_pantilt.c/h`: pan-tilt driver, same transport. Profile `wifi_pantilt`, category `PAN_TILT`, capabilities PAN_LEFT/PAN_RIGHT/TILT_UP/TILT_DOWN/CENTER (v1 ASCII payloads).
- `ROBOT_MAX_DRIVERS` 8 → 12 (7 registered + 2 new + margin); both drivers registered in `device_registry.c`.
- **Dynamic tool catalog (C1 eliminated):** `robot_tools.c` now generates `control_robot` at session.update time from the registered driver catalog — new HAL accessors `robot_hal_get_driver_count()` / `robot_hal_get_driver_at()`, action enum = **union of registered driver capabilities** (the model only sees executable actions), plus `device_type` hint (car/arm/pantilt/ir/generic) and `duration_ms`/`angle_deg`/`axis_id`/`ir_*` parameters. The static `control_ble_device` blob is removed from the catalog; the adapter keeps routing it as a legacy alias for one release (dispatch table now 6 entries).
- `set_device_endpoint` extended: category `arm`/`pantilt` → registers with `wifi_arm`/`wifi_pantilt` (TCP servo protocol regardless of the protocol param, reflected in the success message); adapter parses `angle_deg`/`axis_id` into the HAL params.
- **Gate (hardware):** ESP32 servo controller on TCP answers `ARM_UP`/`GRAB`/`MOVE_AXIS` and pan-tilt `PAN_LEFT`/`CENTER`; verify payload table matches the controller's protocol.

### Phase 7 — Hardening & observability  ✅ delivered (build clean, 0x2e8450 / 27% free)
- **Connection watchdog (per driver):** `robot_hal_execute()` now tracks in-flight operations per driver (`s_driver_inflight[]`, cap `CONFIG_ROBOT_HAL_MAX_INFLIGHT` default 2 — all execute() calls are synchronous and bounded, so the cap only trips under reentrancy/stuck conditions and returns `ESP_ERR_TIMEOUT` with a friendly message). A stuck-connection reaper logs an error if a driver `execute()` exceeds `CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS` (default 3000 ms).
- **Heap/PSRAM budget monitor:** new `main/hal/robot_health.c/h` — low-priority task (PSRAM stack 4096, prio 1, core 1) on `CONFIG_ROBOT_HAL_HEALTH_INTERVAL_S` (default 60 s) logging the memory budget in the `ble_log_memory_snapshot` format (`[HAL_MEM] Internal: Free/Largest/Min | PSRAM: Free/Largest/Min`) plus the registry/driver/present counts and any non-zero in-flight counters.
- **Registry integrity check:** the health task validates every registry entry — alias non-empty, `driver_profile_id` registered in the HAL driver table, endpoint parseable per protocol (BLE: MAC parsed; WiFi: IP+port; IR: TX gpio) — and flags anomalies; the load-time crc32 blob checks stay in `registry_persist`/`ir_rmt`. New accessor `robot_hal_get_device_at(idx)`.
- **Legacy residual paths (narrowed, documented):** the adapter's legacy BLE fallback is reduced to its back-compat contract — it only fires for actions outside the HAL vocabulary (`ROBOT_ACTION_NONE`: SPIN_180 / MOVE_HEAD / SET_AUTONOMOUS_MODE remain legacy-only) or for aliases not registered in the HAL registry (`ESP_ERR_NOT_FOUND`). All other outcomes (timeout, transport, unsupported, in-flight cap) are definitive HAL answers. IR actions never fall back (unchanged).
- **HAL registry authority for aliases:** `handle_set_ble_device_alias` re-runs the idempotent legacy migration after saving, so a renamed device becomes HAL-controllable immediately (no WiFi-reconnect wait). The legacy NVS namespace `ble_profiles` is **kept** as the provisioning store (discovery list + alias persistence + migration source still read/write it); a future Phase 7-b can move provisioning fully into the HAL registry, mirroring the Phase 3-b deferral.
- **Gate:** 72 h soak with audio + intermittent robot power-cycling: zero audio drops, zero heap corruption; health monitor logs show stable Internal/PSRAM budget and zero registry anomalies.

---

## 6. Constraint Enforcement Checklist (traceability)

| Constraint | Enforced by |
|-----------|-------------|
| Audio pipeline never blocked | `robot_hal_execute()` enqueue-and-return; all I/O in driver worker tasks (PSRAM stacks); WebRTC `pc_task` (4 KB) only enqueues. |
| NVS tasks in Internal SRAM | `registry_nvs.c` worker is a clone of `nvs_save_worker_task` (internal stack, core 0, prio 5); lint rule: no `MALLOC_CAP_SPIRAM` in registry_nvs.c. |
| Fail-fast < 500 ms | `CONFIG_ROBOT_PROBE_TIMEOUT_MS` (400 ms default) enforced by `robot_hal` calling `probe()` with deadline + timer; BLE probe = bounded passive scan, no connect. |
| Internal SRAM unfragmented | Driver contexts, registry array, command queues, JSON buffers: `MALLOC_CAP_SPIRAM`. Only registry-NVS worker + FreeRTOS TCBs use internal. |
| ELEGOO BT16 preserved | Phase 1-2 keep `ble_device_control.c` intact; driver wraps it; old NVS namespace read-only fallback. |

---

## 7. Open Questions (to resolve before Phase 1)

1. Tool schema: single `control_robot` (category-aware action filter) vs. one tool per device class? (Recommend: single tool, category filter — fewer tokens, easier schema evolution.)
2. WiFi credentials for LAN robots: reuse existing stored SSID/PSK, or per-endpoint auth field in registry?
3. IR device presence: since IR has no return channel, confirm that `LEARN_IR_CODE` is the only provisioning path (no NVS alias needed for `SEND_IR_COMMAND` beyond a name).
4. Maximum registry size stays 25 devices (matches `BLE_DEVICE_MAX_DEVICES`) — confirm no growth requirement.
