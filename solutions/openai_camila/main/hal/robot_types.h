/**
 * @file robot_types.h
 * @brief Robot HAL core types — protocol-agnostic (Phase 1 skeleton).
 *
 * These types are the normalized vocabulary of the Robot HAL:
 * actions, parameters, results, endpoints and devices. They are
 * transport-independent; concrete drivers map them to BLE / WiFi / IR.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#ifndef ROBOT_TYPES_H
#define ROBOT_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Registry limits ─────────────────────────────────────────────────── */
#define ROBOT_REGISTRY_MAX_DEVICES 25   /* Confirmed cap (matches BLE_DEVICE_MAX_DEVICES) */
#define ROBOT_MAX_DRIVERS         12  /* BLE x2, WiFi x4, IR x3, + margen (Phase 6) */
#define ROBOT_ALIAS_MAX_LEN       32
#define ROBOT_ENDPOINT_MAX_LEN    48
#define ROBOT_PROFILE_ID_MAX_LEN  16

/* ── Transport protocols ─────────────────────────────────────────────── */
typedef enum
{
    ROBOT_PROTOCOL_NONE = 0,
    ROBOT_PROTOCOL_BLE,   /* NimBLE Central (GATT / NUS / serial) */
    ROBOT_PROTOCOL_WIFI,  /* LAN: TCP/UDP, HTTP REST, WebSocket   */
    ROBOT_PROTOCOL_IR     /* ESP-IDF RMT 38 kHz (NEC/Sony/RC5/RAW) */
} robot_protocol_t;

/* ── Device taxonomy ─────────────────────────────────────────────────── */
typedef enum
{
    ROBOT_CATEGORY_CAR = 0,        /* Smart robot cars                    */
    ROBOT_CATEGORY_ARM,            /* Robotic arms (4-DOF / 6-DOF)        */
    ROBOT_CATEGORY_PAN_TILT,       /* Pan-Tilt camera mounts              */
    ROBOT_CATEGORY_IR_ACTUATOR,    /* Generic IR-controlled devices       */
    ROBOT_CATEGORY_LIGHT,          /* Smart lights (BLE/WiFi bulbs/strips)*/
    ROBOT_CATEGORY_GENERIC
} robot_category_t;

/* ── Normalized action IDs (superset covering all device classes) ────── */
typedef enum
{
    ROBOT_ACTION_NONE = 0,
    /* Car */
    ROBOT_ACTION_FORWARD,
    ROBOT_ACTION_BACKWARD,
    ROBOT_ACTION_LEFT,
    ROBOT_ACTION_RIGHT,
    ROBOT_ACTION_STOP,
    ROBOT_ACTION_ROTATE,
    ROBOT_ACTION_READ_ULTRASONIC,
    ROBOT_ACTION_READ_LINE_SENSOR,
    ROBOT_ACTION_READ_BATTERY,
    /* Arm */
    ROBOT_ACTION_GRAB,
    ROBOT_ACTION_RELEASE,
    ROBOT_ACTION_ARM_UP,
    ROBOT_ACTION_ARM_DOWN,
    ROBOT_ACTION_ARM_HOME,
    ROBOT_ACTION_MOVE_AXIS,
    /* Pan-Tilt */
    ROBOT_ACTION_PAN_LEFT,
    ROBOT_ACTION_PAN_RIGHT,
    ROBOT_ACTION_TILT_UP,
    ROBOT_ACTION_TILT_DOWN,
    ROBOT_ACTION_CENTER,
    /* IR */
    ROBOT_ACTION_SEND_IR_COMMAND,
    ROBOT_ACTION_LEARN_IR_CODE,
    /* Light */
    ROBOT_ACTION_TURN_ON,
    ROBOT_ACTION_TURN_OFF,
    ROBOT_ACTION_TOGGLE,
    ROBOT_ACTION_SET_BRIGHTNESS
} robot_action_id_t;

/* ── IR protocols (Phase 5) ──────────────────────────────────────────── */
typedef enum
{
    ROBOT_IR_PROTOCOL_NEC = 0,
    ROBOT_IR_PROTOCOL_SONY,
    ROBOT_IR_PROTOCOL_RC5,
    ROBOT_IR_PROTOCOL_RAW
} robot_ir_protocol_t;

/* ── Normalized action parameters ────────────────────────────────────── */
typedef struct
{
    uint32_t speed;        /* 0..100 (%)                     */
    uint32_t duration_ms;  /* pulse duration / stop-delay    */
    int32_t  angle_deg;    /* 0..180 for arm/pan-tilt        */
    uint8_t  axis_id;      /* 0=base,1=shoulder,2=elbow,3=gripper */
    uint8_t  ir_protocol;  /* robot_ir_protocol_t (NEC|SONY|RC5|RAW) */
    uint32_t ir_address;
    uint32_t ir_command;
    const uint32_t *raw_timings; /* RAW pulse train (PSRAM alloc) */
    uint16_t raw_len;
    uint8_t  brightness_pct; /* 0..100 (%) for lights         */
} robot_action_params_t;

/* ── Result codes ────────────────────────────────────────────────────── */
typedef enum
{
    ROBOT_RESULT_OK = 0,
    ROBOT_RESULT_ERR_NOT_FOUND,   /* alias unknown        */
    ROBOT_RESULT_ERR_OFFLINE,     /* probe failed fast    */
    ROBOT_RESULT_ERR_TIMEOUT,     /* driver timeout       */
    ROBOT_RESULT_ERR_TRANSPORT,   /* connection/write fail*/
    ROBOT_RESULT_ERR_UNSUPPORTED, /* action not in driver capabilities */
    ROBOT_RESULT_ERR_INVALID_ARG
} robot_result_code_t;

typedef struct
{
    robot_result_code_t code;
    char detail[128];      /* human-readable, e.g. "Robot apagado" */
    char telemetry[128];   /* e.g. "25 cm" (ultrasonic)            */
} robot_result_t;

/* ── Endpoint descriptor (protocol-specific, serialized form) ────────── */
typedef struct
{
    char     endpoint[ROBOT_ENDPOINT_MAX_LEN]; /* "AA:BB:..:FF" | "192.168.1.50:8000" | "gpio:17" */
    uint8_t  addr[6];      /* parsed BLE MAC                                 */
    uint8_t  addr_type;    /* BLE address type                              */
    char     ip[16];       /* WiFi endpoint                                 */
    uint16_t port;         /* WiFi endpoint                                 */
    uint8_t  gpio;         /* IR TX pin (RMT)                               */
    char     service_uuid[36]; /* BLE (cached after discovery)          */
    char     char_uuid[36];    /* BLE (cached after discovery)          */
    uint16_t value_handle;  /* BLE (cached after discovery)             */
    uint16_t notify_handle; /* BLE (cached after discovery)             */
    uint16_t cccd_handle;   /* BLE (cached after discovery)             */
} robot_endpoint_t;

/* ── Registry device entry (RAM + NVS serialized) ────────────────────── */
typedef struct
{
    uint32_t         id;      /* stable id = CRC32(endpoint)     */
    char             alias[ROBOT_ALIAS_MAX_LEN]; /* "Carro"     */
    robot_protocol_t protocol;
    robot_category_t category;
    char             driver_profile_id[ROBOT_PROFILE_ID_MAX_LEN]; /* "elegoo_bt16" */
    robot_endpoint_t endpoint;
    /* RAM-only runtime state */
    bool             present;
    uint32_t         last_seen_ms;
} robot_device_t;

#ifdef __cplusplus
}
#endif

#endif /* ROBOT_TYPES_H */
