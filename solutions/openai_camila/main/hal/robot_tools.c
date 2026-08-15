/**
 * @file robot_tools.c
 * @brief Robot tool catalog builder implementation (Phase 1 + Phase 6).
 *
 * Phase 1: the JSON blobs below are copied byte-identical from webrtc.c
 * send_session_update() (C1 decoupling). They define the legacy BLE
 * tool contract exposed to the OpenAI Realtime model.
 *
 * Phase 6: the `control_ble_device` blob is replaced by a dynamic
 * `control_robot` tool generated from the registered HAL driver catalog
 * (action enum = union of driver capabilities, so the model only sees
 * actions some driver can actually execute). C1 is eliminated for the
 * control tool; `control_ble_device` stays as an adapter-level alias for
 * one release.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "robot_tools.h"

#include <string.h>
#include "esp_log.h"
#include <cJSON.h>
#include <stddef.h>
#include "robot_hal.h"

#define TAG "ROBOT_TOOLS"

static const char *const s_robot_tool_jsons[] = {

    /* ── get_discovered_ble_devices ─────────────────────────────────── */
    "{"
    "  \"type\": \"function\","
    "  \"name\": \"get_discovered_ble_devices\","
    "  \"description\": \"Use this tool whenever the user asks what Bluetooth devices are discovered, saved, ready, or nearby. Returns a categorized JSON object listing controllable ready devices, offline configured devices, and raw unprofiled devices.\","
    "  \"parameters\": {"
    "    \"type\": \"object\","
    "    \"properties\": {}"
    "  }"
    "}",

    /* ── set_ble_device_alias ───────────────────────────────────────── */
    "{"
    "  \"type\": \"function\","
    "  \"name\": \"set_ble_device_alias\","
    "  \"description\": \"Use this tool when the user asks to assign a friendly name/alias to a discovered Bluetooth device (e.g., rename ELEGOO BT16 to 'Carro').\","
    "  \"parameters\": {"
    "    \"type\": \"object\","
    "    \"properties\": {"
    "      \"device_name\": {"
    "        \"type\": \"string\","
    "        \"description\": \"Current name or MAC of the device (e.g., 'ELEGOO BT16').\""
    "      },"
    "      \"new_alias\": {"
    "        \"type\": \"string\","
    "        \"description\": \"New user-assigned friendly name (e.g., 'Carro', 'Foco Sala').\""
    "      }"
    "    },"
    "    \"required\": [\"device_name\", \"new_alias\"]"
    "  }"
    "}",

    /* ── set_device_endpoint ────────────────────────────────────────── */
    "{"
    "  \"type\": \"function\","
    "  \"name\": \"set_device_endpoint\","
    "  \"description\": \"Use this tool when the user asks to register, add or update a WiFi/network robot device by IP address and port (e.g., 'registra mi robot en 192.168.1.50 puerto 8000'). The alias is then controllable with control_robot. Category 'arm' and 'pantilt' register TCP servo controllers.\","
    "  \"parameters\": {"
    "    \"type\": \"object\","
    "    \"properties\": {"
    "      \"device_name\": {"
    "        \"type\": \"string\","
    "        \"description\": \"Friendly alias for the device (e.g., 'Robot TCP', 'Brazo', 'Camara').\""
    "      },"
    "      \"ip_address\": {"
    "        \"type\": \"string\","
    "        \"description\": \"IPv4 address of the device on the local network (e.g., 192.168.1.50).\""
    "      },"
    "      \"port\": {"
    "        \"type\": \"integer\","
    "        \"description\": \"TCP port of the device service (default 8000).\""
    "      },"
    "      \"protocol\": {"
    "        \"type\": \"string\","
    "        \"enum\": [\"tcp\", \"http\"],"
    "        \"description\": \"Transport: 'tcp' for raw TCP sockets, 'http' for HTTP REST APIs.\""
    "      },"
    "      \"category\": {"
    "        \"type\": \"string\","
    "        \"enum\": [\"car\", \"arm\", \"pantilt\", \"generic\"],"
    "        \"description\": \"Optional device class (default 'car'). 'arm' and 'pantilt' use the TCP servo-controller protocol.\""
    "      }"
    "    },"
    "    \"required\": [\"device_name\", \"ip_address\", \"port\", \"protocol\"]"
    "  }"
    "}",

    /* ── set_ir_device ───────────────────────────────────────────────── */
    "{"
    "  \"type\": \"function\","
    "  \"name\": \"set_ir_device\","
    "  \"description\": \"Use this tool when the user asks to register an IR-controlled device (TV, air conditioner, fan, soundbar...) by protocol. The device is then controlled with control_robot using SEND_IR_COMMAND (with learned codes or address/command) and LEARN_IR_CODE to capture buttons from the original remote.\","
    "  \"parameters\": {"
    "    \"type\": \"object\","
    "    \"properties\": {"
    "      \"device_name\": {"
    "        \"type\": \"string\","
    "        \"description\": \"Friendly alias for the device (e.g., 'TV Sala', 'Aire').\""
    "      },"
    "      \"ir_protocol\": {"
    "        \"type\": \"string\","
    "        \"enum\": [\"NEC\", \"SONY\", \"RC5\"],"
    "        \"description\": \"IR protocol used by the device (default: NEC).\""
    "      }"
    "    },"
    "    \"required\": [\"device_name\", \"ir_protocol\"]"
    "  }"
    "}"
};

/* ── control_robot: generated from the registered driver catalog ─────── */

static int robot_tools_build_control_robot(cJSON *tools)
{
    uint32_t caps_union = 0;
    const size_t n_drivers = robot_hal_get_driver_count();
    for (size_t i = 0; i < n_drivers; i++)
    {
        const robot_driver_t *drv = robot_hal_get_driver_at(i);
        if (drv != NULL)
        {
            caps_union |= drv->capabilities;
        }
    }

    cJSON *tool = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *props = cJSON_CreateObject();
    cJSON *req = cJSON_CreateArray();
    if (tool == NULL || params == NULL || props == NULL || req == NULL)
    {
        if (tool) cJSON_Delete(tool);
        if (params) cJSON_Delete(params);
        if (props) cJSON_Delete(props);
        if (req) cJSON_Delete(req);
        return -1;
    }

    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "control_robot");
    cJSON_AddStringToObject(tool, "description",
                            "Use this tool to move, control, or read telemetry from any registered robot device: "
                            "Bluetooth cars (e.g. ELEGOO BT16), WiFi/network robots by IP (tcp/http), robotic arms, "
                            "pan-tilt mounts, and IR-controlled devices (NEC/Sony/RC5). "
                            "Available actions depend on the device type.");

    cJSON *dev_name = cJSON_CreateObject();
    cJSON_AddStringToObject(dev_name, "type", "string");
    cJSON_AddStringToObject(dev_name, "description",
                            "Name or alias of the registered device (e.g., 'Carro', 'Brazo', 'TV Sala').");
    cJSON_AddItemToObject(props, "device_name", dev_name);

    cJSON *dev_type = cJSON_CreateObject();
    cJSON_AddStringToObject(dev_type, "type", "string");
    cJSON *dev_type_enum = cJSON_CreateStringArray(
        (const char *const[]){"car", "arm", "pantilt", "ir", "generic"}, 5);
    cJSON_AddItemToObject(dev_type, "enum", dev_type_enum);
    cJSON_AddStringToObject(dev_type, "description",
                            "Optional device class hint: 'car', 'arm', 'pantilt', 'ir', 'generic'.");
    cJSON_AddItemToObject(props, "device_type", dev_type);

    cJSON *action = cJSON_CreateObject();
    cJSON_AddStringToObject(action, "type", "string");
    cJSON *action_enum = cJSON_CreateArray();
    for (int a = ROBOT_ACTION_FORWARD; a <= ROBOT_ACTION_LEARN_IR_CODE; a++)
    {
        if ((caps_union & (1u << (robot_action_id_t)a)) != 0)
        {
            cJSON_AddItemToArray(action_enum,
                                 cJSON_CreateString(robot_action_to_string((robot_action_id_t)a)));
        }
    }
    cJSON_AddItemToObject(action, "enum", action_enum);
    cJSON_AddStringToObject(action, "description",
                            "Action to execute (only actions supported by the device type are listed).");
    cJSON_AddItemToObject(props, "action", action);

    cJSON *duration = cJSON_CreateObject();
    cJSON_AddStringToObject(duration, "type", "integer");
    cJSON_AddStringToObject(duration, "description",
                            "Optional pulse duration in ms for car motion, or parameter value "
                            "(e.g. mode 1 for line follower / 2 for obstacle avoidance in SET_AUTONOMOUS_MODE).");
    cJSON_AddItemToObject(props, "duration_ms", duration);

    cJSON *angle = cJSON_CreateObject();
    cJSON_AddStringToObject(angle, "type", "integer");
    cJSON_AddStringToObject(angle, "description",
                            "Optional servo angle in degrees (0-180) for arm/pan-tilt actions (MOVE_AXIS, ARM_UP, ...).");
    cJSON_AddItemToObject(props, "angle_deg", angle);

    cJSON *axis = cJSON_CreateObject();
    cJSON_AddStringToObject(axis, "type", "integer");
    cJSON_AddStringToObject(axis, "description",
                            "Optional axis id for MOVE_AXIS (0=base, 1=shoulder, 2=elbow, 3=gripper).");
    cJSON_AddItemToObject(props, "axis_id", axis);

    cJSON *ir_proto = cJSON_CreateObject();
    cJSON_AddStringToObject(ir_proto, "type", "string");
    cJSON_AddItemToObject(ir_proto, "enum",
                          cJSON_CreateStringArray((const char *const[]){"NEC", "SONY", "RC5"}, 3));
    cJSON_AddStringToObject(ir_proto, "description",
                            "Optional IR protocol for SEND_IR_COMMAND (default: NEC).");
    cJSON_AddItemToObject(props, "ir_protocol", ir_proto);

    cJSON *ir_addr = cJSON_CreateObject();
    cJSON_AddStringToObject(ir_addr, "type", "integer");
    cJSON_AddStringToObject(ir_addr, "description",
                            "Optional IR address (device code) for SEND_IR_COMMAND. "
                            "If omitted, replays the code learned with LEARN_IR_CODE.");
    cJSON_AddItemToObject(props, "ir_address", ir_addr);

    cJSON *ir_cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(ir_cmd, "type", "integer");
    cJSON_AddStringToObject(ir_cmd, "description",
                            "Optional IR command (function code) for SEND_IR_COMMAND. "
                            "If omitted, replays the code learned with LEARN_IR_CODE.");
    cJSON_AddItemToObject(props, "ir_command", ir_cmd);

    cJSON_AddItemToObject(params, "type", cJSON_CreateString("object"));
    cJSON_AddItemToObject(params, "properties", props);
    cJSON_AddItemToArray(req, cJSON_CreateString("device_name"));
    cJSON_AddItemToArray(req, cJSON_CreateString("action"));
    cJSON_AddItemToObject(params, "required", req);
    cJSON_AddItemToObject(tool, "parameters", params);
    cJSON_AddItemToArray(tools, tool);

    ESP_LOGI(TAG, "control_robot generado desde el catalogo: %d acciones (%d drivers)",
             (int)cJSON_GetArraySize(action_enum), (int)n_drivers);
    return 1;
}

#define ROBOT_TOOL_JSON_COUNT (sizeof(s_robot_tool_jsons) / sizeof(s_robot_tool_jsons[0]))

int robot_tools_append_tools_json(cJSON *tools)
{
    if (tools == NULL)
    {
        return -1;
    }

    int appended = 0;
    for (size_t i = 0; i < ROBOT_TOOL_JSON_COUNT; i++)
    {
        cJSON *tool = cJSON_Parse(s_robot_tool_jsons[i]);
        if (tool)
        {
            cJSON_AddItemToArray(tools, tool);
            appended++;
        }
        else
        {
            ESP_LOGE(TAG, "Fallo parseando herramienta #%d", (int)i);
        }
    }

    const int dyn = robot_tools_build_control_robot(tools);
    if (dyn < 0)
    {
        ESP_LOGE(TAG, "Fallo generando control_robot dinamico");
    }
    else
    {
        appended += dyn;
    }

    ESP_LOGI(TAG, "Catálogo de herramientas: %d añadidas", appended);
    return appended;
}
