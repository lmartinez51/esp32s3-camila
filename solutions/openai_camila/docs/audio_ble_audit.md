# Audit: AFE Audio Stall + BLE Concurrency + GATT Lifecycle

Date: 2026-08-16
Scope: `solutions/openai_camila` (ESP-IDF v5.4.3, ESP32-S3, ESP-BOX-3)
Status: **Findings only — no patches applied.** Fixes proposed below are not yet implemented.

---

## 1. Symptom recap (device log)

```
I (15078) BLE_DEVICE_CTRL: Intentando reconectar con dispositivo conocido: Bathroom
I (15088) BLE_DEVICE_CTRL: Conectando a dispositivo: Bathroom
I (15098) BLE_DEVICE_CTRL: Iniciando escaneo de dispositivos BLE...
I (20818) WEBRTC: WebRTC Connected
I (20888) WEBRTC: Data Channel connected
I (21338) WEBRTC: session.update enviado (intento 1/8); esperando session.updated
I (20098) BLE_DEVICE_CTRL: Evento de conexión recibido (status: 13, handle: 65535)
E (20098) BLE_DEVICE_CTRL: Error en conexión: 13
E (20098) BLE_DEVICE_CTRL: No se pudo encontrar el dispositivo del intento de conexión fallido en la lista.
E (21098) BLE_DEVICE_CTRL: 💥 TIMEOUT: La ráfaga de escaneo 1 no terminó a tiempo.
W (21098) BLE_DEVICE_CTRL: 🔄 Reintento 1/2 del ciclo de descubrimiento completo
E (26078) AFE_VC: Ringbuffer of AFE is full, Please use fetch() ... (spammed indefinitely)
```

Prior incidents (from earlier logs): zombie connection handle 1 → Error 6 on commands, full GATT discovery on every command (+2.5 s), HAL watchdog `Driver tardo X ms (watchdog 3000 ms)`, `TIMEOUT: Quedan -1 operaciones de perfilado sin terminar`.

---

## 2. Audio pipeline — `AFE_VC: Ringbuffer of AFE is full`

### 2.1 Verified pipeline topology

```
I2S/ES7210 → buffer_in (prio 15, core 0)          [feed()]            → AFE task (prio 20, core 1, libesp_audio_front_end)
AUD_SRC thread (prio 16, core 0)                  [fetch()]           ← esp_capture.c:335
AUD_SRC → audio_src_q (data_queue)                                    ← esp_capture.c:779
aenc thread (prio 15, core 0)                     [encode]            ← esp_capture_path_simple.c:234
aenc → audio_q (msg_q, 5 slots) + share_q (5 slots)                   ← esp_capture.c:765 / share_q.c
media_send_task (prio 15, core 1)                 [esp_peer_send_audio] ← esp_webrtc.c:137
```

Every stage is connected by **blocking primitives with no timeout**:

| Primitive | Location | Blocking behavior |
|---|---|---|
| `share_q_add` | `components/esp_capture/src/share_q.c:183` | `pthread_cond_wait` forever when full |
| `msg_q_send` | `components/media_lib_sal/port/msg_q.c` | `pthread_cond_wait` forever when full |
| `data_queue_get_buffer` / `data_queue_read_lock` | `components/media_lib_sal/port/data_queue.c` | event-group wait, effectively forever |

### 2.2 Root-cause chain (verified from source)

1. The RTP send path stalls (the first failure is inside the **precompiled** `esp_peer_send_audio`, `components/esp_webrtc/impl/peer_default/libs/esp32s3/*` — opaque).
2. `media_send_task` blocks forever in `esp_peer_send_audio` (peer `send_queue_num` = 256 metadata slots, `cache_timeout` = 5000 ms defaults from `esp_peer_default.h`).
3. `audio_q` / `share_q` (5 slots each) fill → `aenc` blocks in `msg_q_send` forever.
4. `audio_src_q` fills → `AUD_SRC` blocks in `data_queue_get_buffer` forever.
5. `fetch()` is no longer called → AFE internal ringbuffer fills → `AFE_VC: Ringbuffer of AFE is full` spam **forever** (no recovery path exists in the chain).

### 2.3 Timing correlation

- DC open at 20888 ms → streaming begins.
- 20888 + 256 frames × 20 ms ≈ 26008 ms ≈ 26078 ms (warning start). The 4.7 s delay after `session.update` (21338 ms) is exactly the time needed to fill the 256-slot send queue with 20 ms frames.
- Conclusion: the send queue stopped draining at (or very shortly after) the start of streaming; `session.update` at 21338 ms is coincident, not causal (it is sent from a low-priority task, webrtc.c:755, that does not touch the media path).

### 2.4 What is NOT the cause (verified)

- `session.update` itself: async task, prio 5, core 1, releases mutex after send (webrtc.c:755-800, 1569).
- `vigilante_aec_read_wrapper` mute hack (media_sys.c): writes ±1 PCM, benign to the stall.

### 2.5 Open item

The initial RTP drain trigger cannot be proven from source (precompiled peer lib). BLE radio activity (section 3) overlaps the ignition window but ends ~25.1 s, ~1.7 s before the stall starts — aggravator, not sole cause. **A full device log covering 20.5-26.1 s (and after) is required** to see what fired right before the queue filled (robot HAL command, BLE event, DTLS/SRTP error, or nothing).

### 2.6 Proposed fixes (audio)

- **P1.** Add bounded waits + drop-oldest policy to `share_q_add`, `msg_q_send`, and `data_queue` consumer waits so a stalled send path releases the AFE instead of deadlocking it forever.
- **P1.** Add a watchdog on `media_send_task`: if `esp_peer_send_audio` exceeds N ms, log + drop and re-arm the pipeline (or reset the peer send cache) instead of blocking indefinitely.
- **P2.** Reduce `esp_peer_default` `send_queue_num` from 256 to a smaller value (e.g. 64 ≈ 1.3 s) so stalls surface in seconds, not ~5 s, and recovery latency shrinks.
- **P2.** Verify with the full log whether BLE/WiFi coexistence (2.4 GHz) during the ignition window is degrading RTP throughput; if so, guarantee BLE is fully released (section 3.6) before streaming starts.

---

## 3. BLE concurrency — all root causes confirmed at `solutions/openai_camila/main/ble/ble_device_control.c`

### 3.1 `TIMEOUT: Quedan -1 operaciones de perfilado sin terminar` (signed underflow)

Evidence: `DECR_ACTIVE_OPS()` (:66-74) is called at **three** sites:

- :2107 — failed connect, unknown device found (legit: INCR'd at :3948)
- :2114 — failed connect, device **not found** — **unconditional** (BUG)
- :2218 — disconnect of unknown device (legit)

`INCR_ACTIVE_OPS()` is called at exactly **one** site (:3948, smart-task profiling of an unknown candidate). Known-device reconnects (e.g. `attempt_device_reconnection` → "Bathroom") **never** INCR. When such a connect fails and the lookup misses, :2114 decrements a counter that was never incremented → 0 → −1.

The macro makes the transient visible to concurrent readers:

```c
int old = atomic_fetch_sub(&g_active_ble_operations, 1);
if (old <= 1) { atomic_store(&g_active_ble_operations, 0); }
```

Between `fetch_sub` (produces −1) and the clamping `store(0)`, the smart task's `atomic_load` at :4017 reads −1 → logs `Quedan -1` and calls `force_cleanup_stuck_operations()`.

**Fixes**
- `DECR_ACTIVE_OPS()`: `if (old <= 0) { restore to 0; skip log }` — never decrement below 0; replace the clamp race with a compare-and-swap.
- :2114: only DECR when a profiling op was actually outstanding for this attempt (carry a flag in `g_pending_connection`), instead of unconditionally.

### 3.2 Stale `g_pending_connection.addr` → "No se pudo encontrar el dispositivo..."

Evidence: `g_pending_connection.addr` is populated only at :3938 (smart candidate) and :4650 (on-demand driver path). `attempt_device_reconnection` (:1209) calls `ble_device_connect` (:1247) **without** setting it. The failed-connect handler then looks up the *stale* address (:2100, :2208):

- lookup misses → :2113-2114: error log + unconditional DECR (feeds 3.1);
- lookup hits the wrong (known) device → it gets marked `BLE_DEVICE_STATE_ERROR` incorrectly, and the real failed device keeps its state.

**Fix**
- Populate `g_pending_connection` (addr, addr_type, is_active) inside `ble_device_connect()` itself before `ble_gap_connect()`, and clear it only on the corresponding CONNECT event. Then :2100/:2208 always resolve the correct device.

### 3.3 `TIMEOUT: La ráfaga de escaneo 1 no terminó a tiempo` — EBUSY scan lies

Evidence: `ble_device_start_scan` (:1529):

```c
if (rc == BLE_HS_EBUSY) {
    ESP_LOGD(TAG, "Escaneo omitido: exploracion o conexion GAP activa (EBUSY)");
    return ESP_OK;          // ← NO scan was started
}
scanning_active = true;
```

The observed sequence: :15088 `ble_gap_connect("Bathroom")` pending (5 s timeout) → :15098 burst calls `ble_device_start_scan(5000)` → `ble_gap_disc` returns `BLE_HS_EBUSY` → function returns `ESP_OK` with **no scan running** → smart task waits 6 s on `scan_complete_semaphore` (:3889), which will never be given → :21098 timeout log → `ble_device_stop_scan()` (no-op) → :21098 "Reintento 1/2" with 2 s delay → retry runs **during the WebRTC call**.

**Fixes**
- Return a distinct error (`ESP_ERR_BUSY`) on `BLE_HS_EBUSY`; make the smart-task burst treat it as "burst skipped, cycle completes" without waiting on the semaphore.
- Better: never start a burst while a GAP connect is pending — check `ble_gap_conn_active()` (or the module's own state) before scanning.

### 3.4 GAP operation serialization (scan vs connect)

Evidence: `attempt_device_reconnection` runs **before** the burst in every cycle (:3840 → :3885), so reconnect (`ble_gap_connect`) and burst (`ble_gap_disc`) always overlap when a known device is down. NimBLE rejects the second GAP op (`BLE_HS_EBUSY`), but the code masks it as success (3.3).

**Fix**
- A single GAP state machine in the module: `GAP_IDLE → GAP_SCANNING → GAP_CONNECTING → GAP_CONNECTED`, with every entry point refusing/queueing when not idle. All callers (`attempt_device_reconnection`, smart burst, on-demand, transport) go through it.

### 3.5 WebRTC gating gap

Evidence: `ble_is_webrtc_active()` (:76-85) checks `WEBRTC_CONNECTED_BIT`, set only when the data channel opens (webrtc.c:3032). During the entire WebRTC negotiation (peer connect → ICE/DTLS → DC open) background BLE scan/reconnect is **not** gated. In the observed session the reconnect started before the call, but its 5 s connect timeout + 6 s burst timeout + retry cycle all landed during streaming startup — the most fragile window.

**Fix**
- Gate background BLE work on a "WebRTC starting" state: set the bit at peer-connection establish (or add a second bit cleared only at session end), and/or have the orchestrator hold the smart task stopped from ignition until the BLE release completes.

### 3.6 BLE not fully released before WebRTC

Evidence: the orchestrator has `orchestrator_ble_release_task` → `ble_device_full_release` (orchestrator_tasks.c:214-228), and `ble_device_full_release` (:1480) exists, but in the observed boot the smart task was still mid-cycle when the call started. The "boot-once" exit (:4122-4125) only triggers after a full cycle; a failed cycle (retries) extends BLE activity across the ignition window.

**Fix**
- Force `ble_device_full_release()` to complete (or `ble_device_control_stop()` + scan cancel + `ble_gap_conn_cancel()`) as a hard precondition of WebRTC ignition, with the release awaited by the orchestrator before `start_stream`.

### 3.7 GATT re-discovery on every session (per-command +2.5 s)

Evidence: on `DISCOVERY_COMPLETE` the module disconnects immediately (ble_device_control.c:2327, "nos desconectamos para poder escanear otros dispositivos"). Every command issued while the device is down does: connect → full `start_service_discovery` → write → `ble_gap_terminate` (e.g. :4818). `ble_transport_connect_and_wait` (ble_transport.c:209-242) reuses an existing discovered connection, but only while it stays up — so telemetry polling with dropped links re-discovers each time (~2.5 s, HAL watchdog 3000 ms pressure → `Driver tardo X ms`).

**Fixes**
- Keep the link alive across commands (do not auto-disconnect after `DISCOVERY_COMPLETE` when a driver is active), or
- Cache GATT handles (service/char/CCCD) in NVS keyed by MAC so a reconnect skips discovery, or
- Make `start_service_discovery` incremental (discover only the service UUIDs of interest) instead of full `disc_all`.

### 3.8 Zombie connection handle → Error 6

Evidence: a stale `conn_handle` in the device record made commands fail with `BLE_HS_ENOENT` (Error 6). Mitigations already present: `ble_device_connect` re-validates with `ble_gap_conn_find` (:1634-1650) and `cleanup_stale_connections` (:1256). These are correct; the remaining exposure is the 3.2 stale-addr path marking the wrong device ERROR, which should be fixed together with 3.2.

### 3.9 Security relaxation (note)

`ble_device_control_start` lowers `ble_hs_cfg.sm_sec_lvl` to 0 (:1286-1298) so ELEGOO BT16 notifications are accepted on unencrypted links. Global tradeoff — document it and keep the range minimal (e.g. only downgrade while the ELEGOO session is active).

---

## 4. Confirmed non-issues / already-correct mitigations

- `ble_transport_connect_and_wait` phased timeouts + `ble_gap_conn_cancel` on phase-1 timeout (:4736-4742) and orphan-link termination (:4724-4734) — correct.
- Late-connect cancellation: on-demand caller abandons → CONNECT handler terminates the fresh link (:2065-2070) — correct.
- `devices_mutex` released before `ble_device_connect` in the smart task (:3936-3942) — correct.
- `update_device_state` uses per-state mutex + post-mutex copies (:2248-2318) — correct.
- HAL prefix-stripping / case-insensitive lookup — correct.

---

## 5. Priority-ordered action list

| # | Area | Fix | Priority |
|---|---|---|---|
| 1 | BLE | 3.1: never DECR below 0; DECR only for profiled ops | P1 |
| 2 | BLE | 3.2: populate `g_pending_connection` in `ble_device_connect` | P1 |
| 3 | BLE | 3.3: EBUSY → real error; burst skips without semaphore wait | P1 |
| 4 | BLE | 3.4: serialize GAP ops (scan/connect) | P1 |
| 5 | Audio | 2.6: bounded waits + drop-oldest on share_q/msg_q/data_queue | P1 |
| 6 | BLE | 3.5 + 3.6: gate/stop background BLE before WebRTC ignition | P1 |
| 7 | Audio | 2.6: `media_send_task` watchdog; shrink `send_queue_num` | P2 |
| 8 | BLE | 3.7: GATT handle caching (NVS) or keep links alive | P2 |
| 9 | BLE | 3.9: scope the sm_sec_lvl downgrade | P3 |

---

## 6. Evidence needed to close the open item

Full device log (`idf.py monitor` capture, `DEBUG` level) covering:

- 20 000-27 000 ms: everything between DC open and the first AFE warning (robot HAL commands, BLE events, WiFi events, SRTP/DTLS errors, `esp_webrtc` media logs);
- after 26 078 ms: does the AFE warning ever stop? Does RTP resume? Any watchdog reset?
- whether `media_send_task` logs anything at stall time (`pc_send`/`media_send` debug lines).

With that, the initial RTP drain trigger can be confirmed or the precompiled lib path can be instrumented (e.g. wrapping `esp_peer_send_audio` with a timed wrapper in `esp_webrtc.c`).