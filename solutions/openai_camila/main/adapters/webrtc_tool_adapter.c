/**
 * @file webrtc_tool_adapter.c
 * @brief WebRTC tool dispatch adapter (Phase 1) — C2/C3 decoupling.
 *
 * The BLE tool handlers are moved verbatim out of webrtc.c into a
 * table-driven adapter. Runtime behavior is preserved byte-for-byte:
 * the handlers still call the legacy ble_device_* APIs (compat shim).
 *
 * The control_ble_device handler attempts the Robot HAL normalized path
 * first (robot_hal_execute). The HAL is now the primary multi-protocol
 * control path (BLE / WiFi / IR drivers, Phase 2–6). The legacy BLE
 * path only handles the back-compat contract (Phase 7): actions outside
 * the HAL vocabulary (SPIN_180 / MOVE_HEAD / SET_AUTONOMOUS_MODE) and
 * aliases not yet registered in the HAL registry.
 *
 * Task/stack contract (unchanged from the original):
 *  - Handler context and args_json: PSRAM (MALLOC_CAP_SPIRAM).
 *  - Task stack: PSRAM 12 KB via webrtc_create_psram_task().
 *  - Never runs on the 4 KB WebRTC pc_task.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "webrtc_tool_adapter.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>

#include "webrtc.h"
#include "ui.h"
#include "ble_device_control.h"
#include "ble_hue.h"
#include "ble_generic_nus.h"
#include "robot_hal.h"
#include "registry_migrate.h"
#include "wifi_session_state.h"
#include "wifi_tcp.h"
#include "wifi_http.h"
#include "wifi_arm.h"
#include "wifi_pantilt.h"
#include "ir_nec.h"
#include "ir_sony.h"
#include "ir_rc5.h"
#include "ir_rmt.h"
#include "registry_persist.h"
#include "nvs_setup.h"
#include "network_storage.h"
#include "nvs_flash.h"

#define TAG "ROBOT_ADAPTER"

#define ROBOT_TOOL_TASK_STACK    12288
#define ROBOT_TOOL_TASK_PRIORITY 5
#define ROBOT_TOOL_TASK_CORE     1

typedef struct
{
    char call_id[128];
    char function_name[64];
    char *args_json; /* PSRAM */
} robot_tool_ctx_t;

/* ── Tool handlers (moved verbatim from webrtc.c:2640-2758) ─────────── */

static void handle_get_discovered_ble_devices(const char *call_id, const char *args_json)
{
    ESP_LOGI(TAG, "Llamada a función detectada! Generando resumen de dispositivos BLE para Chatbot...");
    char summary_json[1024] = {0};
    ble_device_get_summary_for_chatbot(summary_json, sizeof(summary_json));
    send_function_output(call_id, summary_json);
    webrtc_request_response_create();
}

static void handle_control_ble_device(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *dev_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "device_name") : NULL;
    cJSON *act_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "action") : NULL;
    cJSON *dur_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "duration_ms") : NULL;
    cJSON *ang_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "angle_deg") : NULL;
    cJSON *axis_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "axis_id") : NULL;
    cJSON *bright_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "brightness_pct") : NULL;
    cJSON *proto_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ir_protocol") : NULL;
    cJSON *addr_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ir_address") : NULL;
    cJSON *cmd_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ir_command") : NULL;

    const char *dev_str = (cJSON_IsString(dev_item) && dev_item->valuestring) ? dev_item->valuestring : "ELEGOO BT16";
    const char *act_str = (cJSON_IsString(act_item) && act_item->valuestring) ? act_item->valuestring : "FORWARD";
    uint32_t dur_val = (cJSON_IsNumber(dur_item)) ? (uint32_t)dur_item->valueint : 0;

    ESP_LOGI(TAG, "🤖 Llamada a control_ble_device: Dispositivo='%s', Acción='%s', Duración=%lu ms",
             dev_str, act_str, dur_val);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: %s -> %s", dev_str, act_str);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    /* 1) Robot HAL normalized path first (drivers land in Phase 2+).
     *    Phase 1: registry is empty ⇒ robot_hal_execute() cannot return
     *    OK, so control always falls through to the legacy BLE path.
     *    Phase 3: OFFLINE (ESP_OK + ROBOT_RESULT_ERR_OFFLINE) is a
     *    definitive fail-fast answer — never fall back to legacy here.
     *    Phase 5: SEND_IR_COMMAND / LEARN_IR_CODE are IR-only actions —
     *    no legacy BLE fallback, the HAL detail is the final answer.
     *    Phase 7: the legacy fallback is narrowed to its back-compat
     *    contract — ONLY actions outside the HAL vocabulary (legacy-only
     *    strings like SPIN_180/MOVE_HEAD) or aliases not registered in the
     *    HAL registry. Every other outcome (timeout, transport, unsupported,
     *    in-flight cap) is a definitive HAL answer. */
    robot_result_t hal_res = {0};
    robot_action_params_t hal_params = {0};
    hal_params.duration_ms = dur_val;
    hal_params.angle_deg = (cJSON_IsNumber(ang_item)) ? (int32_t)ang_item->valueint : 0;
    hal_params.axis_id = (cJSON_IsNumber(axis_item)) ? (uint8_t)axis_item->valueint : 0;
    if (cJSON_IsNumber(bright_item))
    {
        int val = bright_item->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        hal_params.brightness_pct = (uint8_t)val;
    }
    else if (cJSON_IsString(bright_item) && bright_item->valuestring)
    {
        int val = atoi(bright_item->valuestring);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        hal_params.brightness_pct = (uint8_t)val;
    }
    if (cJSON_IsString(proto_item) && proto_item->valuestring)
    {
        if (strcasecmp(proto_item->valuestring, "sony") == 0)
        {
            hal_params.ir_protocol = ROBOT_IR_PROTOCOL_SONY;
        }
        else if (strcasecmp(proto_item->valuestring, "rc5") == 0)
        {
            hal_params.ir_protocol = ROBOT_IR_PROTOCOL_RC5;
        }
        else
        {
            hal_params.ir_protocol = ROBOT_IR_PROTOCOL_NEC;
        }
    }
    hal_params.ir_address = (cJSON_IsNumber(addr_item)) ? (uint32_t)addr_item->valueint : 0;
    hal_params.ir_command = (cJSON_IsNumber(cmd_item)) ? (uint32_t)cmd_item->valueint : 0;

    robot_action_id_t hal_action = robot_action_from_string(act_str);
    const bool is_ir_action = (hal_action == ROBOT_ACTION_SEND_IR_COMMAND ||
                               hal_action == ROBOT_ACTION_LEARN_IR_CODE);
    const bool is_light_action = (hal_action == ROBOT_ACTION_TURN_ON ||
                                  hal_action == ROBOT_ACTION_TURN_OFF ||
                                  hal_action == ROBOT_ACTION_TOGGLE ||
                                  hal_action == ROBOT_ACTION_SET_BRIGHTNESS);
    esp_err_t hal_err = robot_hal_execute(dev_str, hal_action, &hal_params, &hal_res);
    if (hal_err == ESP_OK && hal_res.code == ROBOT_RESULT_OK)
    {
        ui_clear_status_message();
        if (hal_action == ROBOT_ACTION_READ_ULTRASONIC)
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"success\", \"distance\": \"%s\"}",
                     (hal_res.telemetry[0] != '\0') ? hal_res.telemetry : "desconocida");
            send_function_output(call_id, resp_buf);
        }
        else
        {
            const robot_device_t *hal_dev = robot_hal_get_device(dev_str);
            if (hal_dev != NULL && hal_dev->category == ROBOT_CATEGORY_LIGHT)
            {
                char resp_buf[256];
                snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"success\", \"message\": \"%s\"}",
                         (hal_res.detail[0] != '\0') ? hal_res.detail : "Comando de luz ejecutado correctamente");
                send_function_output(call_id, resp_buf);
            }
            else if (hal_dev != NULL && hal_dev->protocol == ROBOT_PROTOCOL_WIFI)
            {
                send_function_output(call_id, "{\"status\": \"success\", \"message\": \"Comando ejecutado correctamente\"}");
            }
            else if (hal_dev != NULL && hal_dev->protocol == ROBOT_PROTOCOL_IR)
            {
                char resp_buf[256];
                snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"success\", \"message\": \"%s\"}",
                         (hal_res.detail[0] != '\0') ? hal_res.detail : "Comando IR ejecutado correctamente");
                send_function_output(call_id, resp_buf);
            }
            else
            {
                send_function_output(call_id, "{\"status\": \"success\", \"message\": \"Comando BLE ejecutado correctamente\"}");
            }
        }
    }
    else if (hal_err == ESP_OK && hal_res.code == ROBOT_RESULT_ERR_OFFLINE)
    {
        ui_clear_status_message();
        ESP_LOGW(TAG, "📡 Probe fail-fast: '%s' respondió OFFLINE", dev_str);
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"El dispositivo está apagado o fuera de alcance (offline).\"}");
    }
    else if (is_ir_action || is_light_action)
    {
        /* IR / Luz: el detalle del HAL es la respuesta definitiva (sin fallback). */
        ui_clear_status_message();
        char resp_buf[256];
        snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"error\", \"message\": \"%s\"}",
                 (hal_res.detail[0] != '\0') ? hal_res.detail : "No se pudo ejecutar el comando");
        send_function_output(call_id, resp_buf);
    }
    else if (hal_action == ROBOT_ACTION_NONE || hal_err == ESP_ERR_NOT_FOUND)
    {
        /* 2) Legacy BLE compat path (Phase 7: narrowed back-compat).
         *    Solo alcanza para acciones fuera del vocabulario HAL
         *    (SPIN_180 / MOVE_HEAD / SET_AUTONOMOUS_MODE, legacy-only)
         *    o para aliases aún no registrados en el registry HAL. */
        esp_err_t ctrl_err = ble_device_send_command_by_alias_or_name(dev_str, act_str, dur_val);
        ui_clear_status_message();
        if (ctrl_err == ESP_OK)
        {
            if (strcasecmp(act_str, "READ_ULTRASONIC") == 0 || strcasecmp(act_str, "leer_ultrasonico") == 0)
            {
                const char *telemetry = ble_device_get_last_telemetry();
                char resp_buf[128];
                snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"success\", \"distance\": \"%s\"}",
                         (telemetry && strlen(telemetry) > 0) ? telemetry : "desconocida");
                send_function_output(call_id, resp_buf);
            }
            else
            {
                send_function_output(call_id, "{\"status\": \"success\", \"message\": \"Comando BLE ejecutado correctamente\"}");
            }
        }
        else
        {
            send_function_output(call_id, "{\"status\": \"error\", \"message\": \"Unable to communicate with the BLE car. Device is offline, powered off, or disconnected.\"}");
        }
    }
    else
    {
        /* 3) Falla definitiva del HAL (sin fallback legacy): timeout,
         *    transporte, no soportado o tope de operaciones en vuelo. */
        ui_clear_status_message();
        char resp_buf[256];
        snprintf(resp_buf, sizeof(resp_buf), "{\"status\": \"error\", \"message\": \"%s\"}",
                 (hal_res.detail[0] != '\0') ? hal_res.detail : "No se pudo ejecutar el comando");
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

static void handle_set_ble_device_alias(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *dev_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "device_name") : NULL;
    cJSON *alias_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "new_alias") : NULL;

    const char *dev_str = (cJSON_IsString(dev_item) && dev_item->valuestring) ? dev_item->valuestring : "";
    const char *alias_str = (cJSON_IsString(alias_item) && alias_item->valuestring) ? alias_item->valuestring : "";

    ESP_LOGI(TAG, "🏷️ Llamada a set_ble_device_alias: Dispositivo='%s', Alias nuevo='%s'", dev_str, alias_str);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: Alias -> %s", alias_str);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    /* 1. Asignar alias en la lista BLE en memoria y encolar NVS */
    esp_err_t alias_err = ble_device_set_alias_by_name(dev_str, alias_str);

    /* 2. Registro directo e inmediato en el HAL registry activo */
    ble_device_info_t *found = ble_device_find_by_name(dev_str);
    if (!found)
    {
        found = ble_device_find_by_name(alias_str);
    }

    if (found != NULL)
    {
        robot_device_t hal_dev = {0};
        strlcpy(hal_dev.alias, alias_str, sizeof(hal_dev.alias));
        hal_dev.protocol = ROBOT_PROTOCOL_BLE;

        bool is_light = (found->type == BLE_DEVICE_TYPE_LIGHT) ||
                        (ble_device_detect_type_from_name(found->name) == BLE_DEVICE_TYPE_LIGHT) ||
                        (ble_device_detect_type_from_name(alias_str) == BLE_DEVICE_TYPE_LIGHT);

        if (is_light)
        {
            hal_dev.category = ROBOT_CATEGORY_LIGHT;
            strlcpy(hal_dev.driver_profile_id, BLE_HUE_PROFILE_ID, sizeof(hal_dev.driver_profile_id));
        }
        else if (strcasestr(found->name, "ELEGOO") != NULL || strcasestr(alias_str, "carro") != NULL)
        {
            hal_dev.category = ROBOT_CATEGORY_CAR;
            strlcpy(hal_dev.driver_profile_id, "elegoo_bt16", sizeof(hal_dev.driver_profile_id));
        }
        else
        {
            hal_dev.category = ROBOT_CATEGORY_GENERIC;
            strlcpy(hal_dev.driver_profile_id, BLE_GENERIC_NUS_PROFILE_ID, sizeof(hal_dev.driver_profile_id));
        }

        memcpy(hal_dev.endpoint.addr, found->addr, 6);
        hal_dev.endpoint.addr_type = found->addr_type;
        snprintf(hal_dev.endpoint.endpoint, sizeof(hal_dev.endpoint.endpoint),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 found->addr[0], found->addr[1], found->addr[2],
                 found->addr[3], found->addr[4], found->addr[5]);
        hal_dev.id = registry_device_id(hal_dev.endpoint.endpoint);
        hal_dev.endpoint.value_handle = found->char_val_handle;

        esp_err_t reg_err = robot_hal_register_device(&hal_dev);
        if (reg_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Dispositivo '%s' registrado en HAL (driver: %s, category: %d)",
                     hal_dev.alias, hal_dev.driver_profile_id, (int)hal_dev.category);
            /* Persistencia en el registry NVS: el alias sobrevive reinicios
             * (trabajador core 0, asincrono — nunca flash en este contexto). */
            esp_err_t persist_err = registry_save_device_async(&hal_dev);
            if (persist_err != ESP_OK)
            {
                ESP_LOGW(TAG, "registry_save_device_async devolvio: %s",
                         esp_err_to_name(persist_err));
            }
        }
        else
        {
            ESP_LOGW(TAG, "robot_hal_register_device devolvio: %s", esp_err_to_name(reg_err));
        }
    }

    ui_clear_status_message();
    if (alias_err == ESP_OK || found != NULL)
    {
        send_function_output(call_id, "{\"status\": \"success\", \"message\": \"Alias guardado y registrado en HAL exitosamente\"}");
    }
    else
    {
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"Dispositivo no encontrado para asignar alias\"}");
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

/* ── Dispatch table ──────────────────────────────────────────────────── */

static bool adapter_parse_ipv4(const char *ip)
{
    int a = 0, b = 0, c = 0, d = 0;
    char extra = '\0';
    if (ip == NULL || ip[0] == '\0')
    {
        return false;
    }
    if (sscanf(ip, "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) != 4)
    {
        return false;
    }
    return a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
           c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static void handle_set_device_endpoint(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *dev_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "device_name") : NULL;
    cJSON *ip_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ip_address") : NULL;
    cJSON *port_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "port") : NULL;
    cJSON *proto_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "protocol") : NULL;
    cJSON *cat_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "category") : NULL;

    const char *alias = (cJSON_IsString(dev_item) && dev_item->valuestring) ? dev_item->valuestring : "";
    const char *ip = (cJSON_IsString(ip_item) && ip_item->valuestring) ? ip_item->valuestring : "";
    const char *proto = (cJSON_IsString(proto_item) && proto_item->valuestring) ? proto_item->valuestring : "tcp";
    const char *cat = (cJSON_IsString(cat_item) && cat_item->valuestring) ? cat_item->valuestring : "car";
    const int port = (cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535)
                         ? port_item->valueint : 8000;

    ESP_LOGI(TAG, "🌐 Llamada a set_device_endpoint: Alias='%s', IP='%s', Puerto=%d, Proto='%s'",
             alias, ip, port, proto);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: Endpoint %s", alias);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    if (alias[0] == '\0' || !adapter_parse_ipv4(ip))
    {
        ui_clear_status_message();
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"Alias o IP invalida (formato esperado: 192.168.1.50).\"}");
        if (args_root) cJSON_Delete(args_root);
        webrtc_request_response_create();
        return;
    }

    const bool is_http = (strcasecmp(proto, "http") == 0);

    /* Phase 6: categoria define el driver (arm/pantilt = TCP servo). */
    const char *profile_id = is_http ? WIFI_HTTP_PROFILE_ID : WIFI_TCP_PROFILE_ID;
    robot_category_t category = ROBOT_CATEGORY_CAR;
    const char *transport_name = is_http ? "http" : "tcp";
    if (strcasecmp(cat, "generic") == 0)
    {
        category = ROBOT_CATEGORY_GENERIC;
    }
    else if (strcasecmp(cat, "light") == 0)
    {
        category = ROBOT_CATEGORY_LIGHT;
    }
    else if (strcasecmp(cat, "arm") == 0)
    {
        category = ROBOT_CATEGORY_ARM;
        profile_id = WIFI_ARM_PROFILE_ID;
        transport_name = "tcp";
    }
    else if (strcasecmp(cat, "pantilt") == 0)
    {
        category = ROBOT_CATEGORY_PAN_TILT;
        profile_id = WIFI_PANTILT_PROFILE_ID;
        transport_name = "tcp";
    }

    robot_device_t dev = {0};
    strlcpy(dev.alias, alias, sizeof(dev.alias));
    dev.protocol = ROBOT_PROTOCOL_WIFI;
    dev.category = category;
    strlcpy(dev.driver_profile_id, profile_id, sizeof(dev.driver_profile_id));
    strlcpy(dev.endpoint.ip, ip, sizeof(dev.endpoint.ip));
    dev.endpoint.port = (uint16_t)port;
    snprintf(dev.endpoint.endpoint, sizeof(dev.endpoint.endpoint), "%s:%u", ip, port);
    dev.id = registry_device_id(dev.endpoint.endpoint);

    esp_err_t err = robot_hal_register_device(&dev);
    if (err == ESP_OK)
    {
        err = registry_save_device_async(&dev); /* persiste en NVS (trabajador core 0) */
    }
    ui_clear_status_message();

    if (err == ESP_OK)
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"success\", \"message\": \"Dispositivo '%s' registrado en %s:%u (%s)\"}",
                 alias, ip, port, transport_name);
        send_function_output(call_id, resp_buf);
    }
    else
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"error\", \"message\": \"No se pudo registrar '%s': %s\"}",
                 alias, esp_err_to_name(err));
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

static void handle_set_ir_device(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *dev_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "device_name") : NULL;
    cJSON *proto_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ir_protocol") : NULL;

    const char *alias = (cJSON_IsString(dev_item) && dev_item->valuestring) ? dev_item->valuestring : "";
    const char *proto = (cJSON_IsString(proto_item) && proto_item->valuestring) ? proto_item->valuestring : "NEC";

    ESP_LOGI(TAG, "📡 Llamada a set_ir_device: Alias='%s', Protocolo='%s'", alias, proto);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: IR %s", alias);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    if (alias[0] == '\0')
    {
        ui_clear_status_message();
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"Nombre de dispositivo invalido.\"}");
        if (args_root) cJSON_Delete(args_root);
        webrtc_request_response_create();
        return;
    }

    const char *profile_id = IR_NEC_PROFILE_ID;
    const char *proto_name = "NEC";
    if (strcasecmp(proto, "sony") == 0)
    {
        profile_id = IR_SONY_PROFILE_ID;
        proto_name = "Sony";
    }
    else if (strcasecmp(proto, "rc5") == 0)
    {
        profile_id = IR_RC5_PROFILE_ID;
        proto_name = "RC5";
    }

    robot_device_t dev = {0};
    strlcpy(dev.alias, alias, sizeof(dev.alias));
    dev.protocol = ROBOT_PROTOCOL_IR;
    dev.category = ROBOT_CATEGORY_IR_ACTUATOR;
    strlcpy(dev.driver_profile_id, profile_id, sizeof(dev.driver_profile_id));
    snprintf(dev.endpoint.endpoint, sizeof(dev.endpoint.endpoint), "gpio:%u",
             CONFIG_ROBOT_IR_TX_GPIO);
    dev.endpoint.gpio = CONFIG_ROBOT_IR_TX_GPIO;
    dev.id = registry_device_id(dev.endpoint.endpoint);

    esp_err_t err = robot_hal_register_device(&dev);
    if (err == ESP_OK)
    {
        err = registry_save_device_async(&dev); /* persiste en NVS (trabajador core 0) */
    }
    ui_clear_status_message();

    if (err == ESP_OK)
    {
        char resp_buf[224];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"success\", \"message\": \"Dispositivo IR '%s' registrado (protocolo %s). Usa control_ble_device con LEARN_IR_CODE para capturar botones del mando.\"}",
                 alias, proto_name);
        send_function_output(call_id, resp_buf);
    }
    else
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"error\", \"message\": \"No se pudo registrar '%s': %s\"}",
                 alias, esp_err_to_name(err));
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

typedef struct
{
    const char *name;
    void (*handler)(const char *call_id, const char *args_json);
} robot_tool_handler_t;

/* ── Removal tools (Phase 8: erase lifecycle) ────────────────────────── */

static void handle_remove_device(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *dev_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "device_name") : NULL;
    const char *alias = (cJSON_IsString(dev_item) && dev_item->valuestring) ? dev_item->valuestring : "";

    ESP_LOGI(TAG, "🗑️ Llamada a remove_device: Dispositivo='%s'", alias);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: Remove %s", alias);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    if (alias[0] == '\0')
    {
        ui_clear_status_message();
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"Nombre de dispositivo invalido.\"}");
        if (args_root) cJSON_Delete(args_root);
        webrtc_request_response_create();
        return;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        ui_clear_status_message();
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"El dispositivo no esta registrado.\"}");
        if (args_root) cJSON_Delete(args_root);
        webrtc_request_response_create();
        return;
    }

    const uint32_t dev_id = dev->id;
    const robot_protocol_t proto = dev->protocol;
    uint8_t mac[6];
    memcpy(mac, dev->endpoint.addr, sizeof(mac));

    /* 1. Registry RAM (HAL) */
    const esp_err_t unreg_err = robot_hal_unregister_device(alias);

    /* 2. Registry NVS (trabajador core 0, asincrono) */
    esp_err_t persist_err = registry_delete_device_async(dev_id);
    if (persist_err == ESP_ERR_INVALID_STATE)
    {
        persist_err = ESP_OK; /* sin worker: nada persistido */
    }

    /* 3. Codigos IR aprendidos del dispositivo (cache + NVS) */
    if (proto == ROBOT_PROTOCOL_IR)
    {
        ir_learn_delete(dev_id);
    }

    /* 4. Perfil BLE legacy (clave D_* en ble_devices, de cualquier SSID) */
    if (proto == ROBOT_PROTOCOL_BLE)
    {
        delete_device_profile_by_mac(mac);
    }

    ui_clear_status_message();
    if (unreg_err == ESP_OK && persist_err == ESP_OK)
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"success\", \"message\": \"Dispositivo '%s' eliminado (registry, NVS y codigos IR).\"}",
                 alias);
        send_function_output(call_id, resp_buf);
    }
    else
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"error\", \"message\": \"No se pudo eliminar '%s': %s (%s)\"}",
                 alias, esp_err_to_name(unreg_err), esp_err_to_name(persist_err));
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

static void handle_forget_wifi_network(const char *call_id, const char *args_json)
{
    cJSON *args_root = args_json ? cJSON_Parse(args_json) : NULL;
    cJSON *ssid_item = args_root ? cJSON_GetObjectItemCaseSensitive(args_root, "ssid") : NULL;
    const char *ssid = (cJSON_IsString(ssid_item) && ssid_item->valuestring) ? ssid_item->valuestring : "";

    ESP_LOGI(TAG, "📡 Llamada a forget_wifi_network: SSID='%s'", ssid);

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: Forget %s", ssid);
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    if (ssid[0] == '\0')
    {
        ui_clear_status_message();
        send_function_output(call_id, "{\"status\": \"error\", \"message\": \"SSID invalido.\"}");
        if (args_root) cJSON_Delete(args_root);
        webrtc_request_response_create();
        return;
    }

    const bool deleted = network_delete_wifi_credential_by_ssid(ssid);
    const char *current = wifi_session_get_connected_ssid();
    const bool is_current = (current != NULL && strcmp(current, ssid) == 0);

    ui_clear_status_message();
    if (deleted)
    {
        char resp_buf[256];
        if (is_current)
        {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"status\": \"success\", \"message\": \"Red '%s' olvidada. Se aplicara al reconectar.\"}",
                     ssid);
        }
        else
        {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"status\": \"success\", \"message\": \"Red '%s' olvidada.\"}", ssid);
        }
        send_function_output(call_id, resp_buf);
    }
    else
    {
        char resp_buf[192];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"error\", \"message\": \"No se encontro la red '%s' en las credenciales guardadas.\"}",
                 ssid);
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();

    if (args_root) cJSON_Delete(args_root);
}

static void handle_erase_all_data(const char *call_id, const char *args_json)
{
    (void)args_json;
    ESP_LOGI(TAG, "🧹 Llamada a erase_all_data: borrando registry, IR, WiFi y BLE");

    char ui_sub[64];
    snprintf(ui_sub, sizeof(ui_sub), "EXECUTING: Erase all data");
    ui_show_status_message(ui_sub, COLOR_GREEN_BGR565);

    /* 1. Registry NVS (trabajador core 0, asincrono) */
    esp_err_t reg_err = registry_persist_erase_all();
    if (reg_err == ESP_ERR_INVALID_STATE)
    {
        reg_err = ESP_OK; /* sin worker: nada persistido */
    }

    /* 2. Codigos IR aprendidos (cache + NVS, asincrono; no-op sin motor) */
    const esp_err_t ir_err = ir_learn_delete_all();

    /* 3. Credenciales WiFi (sincrono, con lock) */
    esp_err_t wifi_err = network_delete_all_wifi_credentials();
    if (wifi_err == ESP_ERR_NVS_NOT_FOUND)
    {
        wifi_err = ESP_OK;
    }

    /* 4. Perfiles BLE legacy (sync, con lock): blobs D_* + base de perfiles */
    nvs_wipe_ble_devices();
    nvs_wipe_ble_profiles();

    /* 5. Registry RAM (HAL): sin dispositivos controlables */
    esp_err_t hal_err = robot_hal_clear_devices();
    if (hal_err == ESP_ERR_INVALID_STATE)
    {
        hal_err = ESP_OK; /* HAL nunca inicializado: nada que limpiar */
    }

    ui_clear_status_message();
    if (reg_err == ESP_OK && ir_err == ESP_OK && wifi_err == ESP_OK && hal_err == ESP_OK)
    {
        send_function_output(call_id, "{\"status\": \"success\", \"message\": \"Todos los datos del robot han sido borrados: dispositivos, codigos IR, redes WiFi y perfiles BLE.\"}");
    }
    else
    {
        char resp_buf[256];
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\": \"error\", \"message\": \"Borrado parcial: registry=%s, IR=%s, WiFi=%s, HAL=%s\"}",
                 esp_err_to_name(reg_err), esp_err_to_name(ir_err),
                 esp_err_to_name(wifi_err), esp_err_to_name(hal_err));
        send_function_output(call_id, resp_buf);
    }
    webrtc_request_response_create();
}

static const robot_tool_handler_t s_robot_tools[] = {
    {"get_discovered_ble_devices", handle_get_discovered_ble_devices},
    {"control_robot",              handle_control_ble_device}, /* Phase 6: catalog name */
    {"control_ble_device",         handle_control_ble_device}, /* legacy alias (una release) */
    {"set_ble_device_alias",       handle_set_ble_device_alias},
    {"set_device_endpoint",        handle_set_device_endpoint},
    {"set_ir_device",              handle_set_ir_device},
    {"remove_device",              handle_remove_device},
    {"forget_wifi_network",        handle_forget_wifi_network},
    {"erase_all_data",             handle_erase_all_data},
};

#define ROBOT_TOOL_COUNT (sizeof(s_robot_tools) / sizeof(s_robot_tools[0]))

/* ── Task wrapper (moved verbatim from webrtc.c, PSRAM stack) ────────── */

static void robot_tool_handler_task(void *arg)
{
    robot_tool_ctx_t *ctx = (robot_tool_ctx_t *)arg;
    if (!ctx)
    {
        vTaskDelete(NULL);
        return;
    }

    const robot_tool_handler_t *tool = NULL;
    for (size_t i = 0; i < ROBOT_TOOL_COUNT; i++)
    {
        if (strcmp(s_robot_tools[i].name, ctx->function_name) == 0)
        {
            tool = &s_robot_tools[i];
            break;
        }
    }

    if (tool)
    {
        tool->handler(ctx->call_id, ctx->args_json);
    }
    else
    {
        ESP_LOGW(TAG, "robot_tool_handler_task: función desconocida: %s", ctx->function_name);
    }

    if (ctx->args_json) free(ctx->args_json);
    free(ctx);
    vTaskDelete(NULL);
}

static void webrtc_tool_adapter_start_task(const char *call_id, const char *function_name, const char *args_json)
{
    if (!call_id) return;

    robot_tool_ctx_t *ctx = heap_caps_malloc(sizeof(robot_tool_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) return;

    strlcpy(ctx->call_id, call_id, sizeof(ctx->call_id));
    if (function_name)
    {
        strlcpy(ctx->function_name, function_name, sizeof(ctx->function_name));
    }
    else
    {
        ctx->function_name[0] = '\0';
    }

    if (args_json)
    {
        size_t len = strlen(args_json) + 1;
        ctx->args_json = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ctx->args_json)
        {
            memcpy(ctx->args_json, args_json, len);
        }
    }
    else
    {
        ctx->args_json = NULL;
    }

    /* Stack PSRAM de 12 KB: suficiente para la cadena BLE + NVS(encolado) + LCD. */
    if (webrtc_create_psram_task(robot_tool_handler_task, "robot_tool",
                                 ROBOT_TOOL_TASK_STACK, ctx,
                                 ROBOT_TOOL_TASK_PRIORITY, NULL,
                                 ROBOT_TOOL_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create robot_tool_handler_task");
        if (ctx->args_json) free(ctx->args_json);
        free(ctx);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool webrtc_tool_adapter_init(void)
{
    ESP_LOGI(TAG, "Robot tool adapter inicializado (%d herramientas)", (int)ROBOT_TOOL_COUNT);
    return true;
}

bool webrtc_tool_adapter_route(const char *function_name, const char *call_id, const char *args_json)
{
    if (!function_name)
    {
        return false;
    }

    for (size_t i = 0; i < ROBOT_TOOL_COUNT; i++)
    {
        if (strcmp(s_robot_tools[i].name, function_name) == 0)
        {
            webrtc_tool_adapter_start_task(call_id, function_name, args_json);
            return true;
        }
    }
    return false;
}
