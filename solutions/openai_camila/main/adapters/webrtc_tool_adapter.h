/**
 * @file webrtc_tool_adapter.h
 * @brief WebRTC tool dispatch adapter (Phase 1) — decouples WebRTC from
 * concrete device drivers (C2/C3).
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef WEBRTC_TOOL_ADAPTER_H
#define WEBRTC_TOOL_ADAPTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Initialize the adapter (registers the tool dispatch table).
 * @return true on success.
 */
bool webrtc_tool_adapter_init(void);

/**
 * @brief Route a WebRTC function call to a registered tool handler.
 *
 * Spawns a dedicated task (PSRAM stack) and returns immediately; the
 * handler executes off the 4 KB WebRTC pc_task.
 *
 * @param function_name Tool name (e.g. "control_ble_device").
 * @param call_id       WebRTC call_id for the function_call.
 * @param args_json     Raw "arguments" JSON string (copied to PSRAM).
 * @return true if the tool was dispatched (handled), false otherwise.
 */
bool webrtc_tool_adapter_route(const char *function_name,
                               const char *call_id,
                               const char *args_json);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_TOOL_ADAPTER_H */
