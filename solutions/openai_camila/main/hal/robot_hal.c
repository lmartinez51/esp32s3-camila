/**
 * @file robot_hal.c
 * @brief Robot HAL facade implementation (Phase 1 skeleton).
 *
 * Registry array and device entries live in PSRAM (MALLOC_CAP_SPIRAM) to
 * keep Internal SRAM unfragmented. The driver table is a small static
 * array of pointers (bounded by ROBOT_MAX_DRIVERS).
 *
 * Phase 1: the registry starts empty and no drivers are registered yet,
 * so robot_hal_execute() always resolves to NOT_FOUND / NOT_SUPPORTED —
 * the WebRTC tool adapter then falls back to the legacy BLE path.
 *
 * @author Lorenzo Martínez
 * @date 2026
 * @version 1.0
 * @platform ESP32-S3-BOX3
 */

#include "robot_hal.h"

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "ROBOT_HAL"

#if !defined(CONFIG_ROBOT_PROBE_CACHE_MS)
#define CONFIG_ROBOT_PROBE_CACHE_MS 10000
#endif
#if !defined(CONFIG_ROBOT_HAL_MAX_INFLIGHT)
#define CONFIG_ROBOT_HAL_MAX_INFLIGHT 2
#endif
#ifdef CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS
#undef CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS
#endif
#define CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS 6000

static SemaphoreHandle_t s_registry_mutex = NULL;
static robot_device_t *s_devices = NULL; /* PSRAM-backed registry array */
static size_t s_device_count = 0;
static const robot_driver_t *s_drivers[ROBOT_MAX_DRIVERS] = {NULL};
static size_t s_driver_count = 0;
static volatile uint32_t s_driver_inflight[ROBOT_MAX_DRIVERS] = {0}; /* Phase 7 */

static int robot_hal_find_driver_idx(const char *profile_id)
{
    if (!profile_id)
    {
        return -1;
    }
    for (size_t i = 0; i < s_driver_count; i++)
    {
        if (s_drivers[i] && strcmp(s_drivers[i]->profile_id, profile_id) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t robot_hal_init(void)
{
    if (s_registry_mutex == NULL)
    {
        s_registry_mutex = xSemaphoreCreateMutex();
        if (s_registry_mutex == NULL)
        {
            ESP_LOGE(TAG, "Fallo creando mutex del registro de dispositivos");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_devices == NULL)
    {
        s_devices = heap_caps_calloc(ROBOT_REGISTRY_MAX_DEVICES, sizeof(robot_device_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_devices == NULL)
        {
            ESP_LOGE(TAG, "Fallo asignando registro de dispositivos en PSRAM (%d x %u bytes)",
                     (int)ROBOT_REGISTRY_MAX_DEVICES, (unsigned)sizeof(robot_device_t));
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Registro de dispositivos asignado en PSRAM (%d entradas)",
                 (int)ROBOT_REGISTRY_MAX_DEVICES);

        /* Una sola vez: robot_hal_init() se re-entra desde register_driver/
         * register_device; el log solo debe salir en el primer arranque. */
        ESP_LOGI(TAG, "Robot HAL inicializado (max %d dispositivos, %d drivers)",
                 (int)ROBOT_REGISTRY_MAX_DEVICES, (int)ROBOT_MAX_DRIVERS);
    }

    return ESP_OK;
}

esp_err_t robot_hal_register_driver(const robot_driver_t *drv)
{
    if (!drv || drv->profile_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = robot_hal_init();
    if (err != ESP_OK)
    {
        return err;
    }

    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < s_driver_count; i++)
    {
        if (s_drivers[i] && strcmp(s_drivers[i]->profile_id, drv->profile_id) == 0)
        {
            xSemaphoreGive(s_registry_mutex);
            ESP_LOGW(TAG, "Driver '%s' ya registrado", drv->profile_id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_driver_count >= ROBOT_MAX_DRIVERS)
    {
        xSemaphoreGive(s_registry_mutex);
        ESP_LOGE(TAG, "Tabla de drivers llena (%d)", (int)ROBOT_MAX_DRIVERS);
        return ESP_ERR_NO_MEM;
    }

    s_drivers[s_driver_count++] = drv;
    xSemaphoreGive(s_registry_mutex);

    ESP_LOGI(TAG, "Driver '%s' registrado (protocol=%d, category=%d)",
             drv->profile_id, (int)drv->protocol, (int)drv->category);
    return ESP_OK;
}

esp_err_t robot_hal_register_device(const robot_device_t *dev)
{
    if (!dev || dev->alias[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = robot_hal_init();
    if (err != ESP_OK)
    {
        return err;
    }

    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < s_device_count; i++)
    {
        if (strcmp(s_devices[i].alias, dev->alias) == 0)
        {
            s_devices[i] = *dev; /* update in place */
            xSemaphoreGive(s_registry_mutex);
            ESP_LOGI(TAG, "Dispositivo '%s' actualizado en el registro", dev->alias);
            return ESP_OK;
        }
    }

    if (s_device_count >= ROBOT_REGISTRY_MAX_DEVICES)
    {
        xSemaphoreGive(s_registry_mutex);
        ESP_LOGE(TAG, "Registro lleno (%d dispositivos), '%s' no registrado",
                 (int)ROBOT_REGISTRY_MAX_DEVICES, dev->alias);
        return ESP_ERR_NO_MEM;
    }

    s_devices[s_device_count++] = *dev;
    xSemaphoreGive(s_registry_mutex);

    ESP_LOGI(TAG, "Dispositivo '%s' registrado (protocol=%d, driver='%s')",
             dev->alias, (int)dev->protocol, dev->driver_profile_id);
    return ESP_OK;
}

static const char *robot_hal_normalize_alias(const char *input)
{
    if (!input) return "";

    while (*input == ' ' || *input == '\t') {
        input++;
    }

    static const char *prefixes[] = {
        "foco de la ", "foco de los ", "foco del ", "foco de ", "foco ",
        "luz de la ", "luz de los ", "luz del ", "luz de ", "luz ",
        "lampara de la ", "lampara del ", "lampara de ", "lampara ",
        "el ", "la ", "los ", "las ", "un ", "una "
    };

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncasecmp(input, prefixes[i], len) == 0) {
            return input + len;
        }
    }
    return input;
}

static bool robot_hal_alias_matches(const char *registered, const char *search)
{
    if (!registered || !search || registered[0] == '\0' || search[0] == '\0') {
        return false;
    }

    /* 1. Coincidencia case-insensitive directa */
    if (strcasecmp(registered, search) == 0) {
        return true;
    }

    /* 2. Coincidencia normalizada sin prefijos conversacionales */
    const char *norm_reg = robot_hal_normalize_alias(registered);
    const char *norm_search = robot_hal_normalize_alias(search);

    if (strcasecmp(norm_reg, norm_search) == 0) {
        return true;
    }

    /* 3. Subcadena en cualquier direccion */
    if ((strlen(norm_search) >= 3 && strcasestr(norm_reg, norm_search) != NULL) ||
        (strlen(norm_reg) >= 3 && strcasestr(norm_search, norm_reg) != NULL)) {
        return true;
    }

    return false;
}

const robot_device_t *robot_hal_get_device(const char *alias)
{
    if (!alias || s_devices == NULL)
    {
        return NULL;
    }

    const robot_device_t *found = NULL;
    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        /* Pase 1: Coincidencia exacta / directa por alias o endpoint */
        for (size_t i = 0; i < s_device_count; i++)
        {
            if (strcasecmp(s_devices[i].alias, alias) == 0 ||
                strcasecmp(s_devices[i].endpoint.endpoint, alias) == 0)
            {
                found = &s_devices[i];
                break;
            }
        }

        /* Pase 2: Coincidencia difusa / normalizada (ej. "Luz de la regadera" -> "Regadera") */
        if (found == NULL)
        {
            for (size_t i = 0; i < s_device_count; i++)
            {
                if (robot_hal_alias_matches(s_devices[i].alias, alias) ||
                    robot_hal_alias_matches(s_devices[i].endpoint.endpoint, alias))
                {
                    found = &s_devices[i];
                    ESP_LOGI(TAG, "Dispositivo '%s' encontrado por coincidencia difusa con '%s'",
                             s_devices[i].alias, alias);
                    break;
                }
            }
        }
        xSemaphoreGive(s_registry_mutex);
    }
    return found;
}

const robot_device_t *robot_hal_get_device_at(size_t idx)
{
    if (s_devices == NULL)
    {
        return NULL;
    }

    const robot_device_t *found = NULL;
    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        if (idx < s_device_count)
        {
            found = &s_devices[idx];
        }
        xSemaphoreGive(s_registry_mutex);
    }
    return found;
}

esp_err_t robot_hal_set_device_presence(const char *alias, bool present)
{
    if (!alias)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < s_device_count; i++)
    {
        if (strcmp(s_devices[i].alias, alias) == 0)
        {
            s_devices[i].present = present;
            s_devices[i].last_seen_ms = present ? (uint32_t)(esp_timer_get_time() / 1000ULL) : 0;
            xSemaphoreGive(s_registry_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_registry_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t robot_hal_get_device_count(void)
{
    return s_device_count;
}

size_t robot_hal_get_driver_count(void)
{
    return s_driver_count;
}

const robot_driver_t *robot_hal_get_driver_at(size_t idx)
{
    if (idx >= s_driver_count)
    {
        return NULL;
    }
    return s_drivers[idx];
}

uint32_t robot_hal_get_driver_inflight_at(size_t idx)
{
    if (idx >= s_driver_count)
    {
        return 0;
    }
    return s_driver_inflight[idx];
}

esp_err_t robot_hal_execute(const char *alias,
                            robot_action_id_t action,
                            const robot_action_params_t *params,
                            robot_result_t *out)
{
    if (!alias || !out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->code = ROBOT_RESULT_ERR_NOT_FOUND;

    if (action == ROBOT_ACTION_NONE)
    {
        out->code = ROBOT_RESULT_ERR_INVALID_ARG;
        snprintf(out->detail, sizeof(out->detail), "Accion no reconocida o fuera del vocabulario HAL");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = robot_hal_init();
    if (err != ESP_OK)
    {
        out->code = ROBOT_RESULT_ERR_TRANSPORT;
        return err;
    }

    const robot_device_t *dev = robot_hal_get_device(alias);
    if (dev == NULL)
    {
        snprintf(out->detail, sizeof(out->detail),
                 "Dispositivo '%s' no registrado en el HAL", alias);
        return ESP_ERR_NOT_FOUND;
    }

    const int drv_idx = robot_hal_find_driver_idx(dev->driver_profile_id);
    robot_driver_t *drv = (drv_idx >= 0) ? (robot_driver_t *)s_drivers[drv_idx] : NULL;
    if (drv == NULL || drv->execute == NULL)
    {
        out->code = ROBOT_RESULT_ERR_UNSUPPORTED;
        snprintf(out->detail, sizeof(out->detail),
                 "Sin driver '%s' disponible para '%s'", dev->driver_profile_id, alias);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Phase 7: tope de operaciones en vuelo por driver (protector del
     * presupuesto acotado). Los execute son sincronos y acotados, asi que
     * el tope solo dispara en condiciones anormales (reentrada, colgado). */
    if (s_driver_inflight[drv_idx] >= (uint32_t)CONFIG_ROBOT_HAL_MAX_INFLIGHT)
    {
        out->code = ROBOT_RESULT_ERR_TIMEOUT;
        snprintf(out->detail, sizeof(out->detail),
                 "Driver '%s' ocupado (%u operaciones en vuelo), reintenta en un momento",
                 drv->profile_id, (unsigned)s_driver_inflight[drv_idx]);
        return ESP_ERR_TIMEOUT;
    }

    /* Fail-fast probe (Phase 3): skip only while the presence cache is
     * fresh. Absent results are never cached, so an offline device gets a
     * <500 ms answer on every command. */
    if (drv->probe != NULL)
    {
        bool need_probe = true;
        if (xSemaphoreTake(s_registry_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            need_probe = (dev->last_seen_ms == 0) ||
                         (now_ms - dev->last_seen_ms > (uint32_t)CONFIG_ROBOT_PROBE_CACHE_MS);
            xSemaphoreGive(s_registry_mutex);
        }

        if (need_probe)
        {
            bool present = false;
            esp_err_t perr = drv->probe(drv, &dev->endpoint, &present);
            if (perr == ESP_OK && !present)
            {
                robot_hal_set_device_presence(alias, false);
                out->code = ROBOT_RESULT_ERR_OFFLINE;
                snprintf(out->detail, sizeof(out->detail),
                         "El dispositivo '%s' no esta encendido o fuera de alcance", alias);
                return ESP_OK; /* handled: caller must NOT fall back */
            }
            if (perr == ESP_OK && present)
            {
                robot_hal_set_device_presence(alias, true);
            }
        }
    }

    /* Phase 7: watchdog de conexion colgada — cronometra el execute y
     * avisa si un driver excede el presupuesto (los drivers acotan con
     * timeouts propios; este log detecta regresiones). */
    const int64_t t0 = esp_timer_get_time();
    s_driver_inflight[drv_idx]++;
    esp_err_t xerr = drv->execute(drv, alias, action, params, out);
    s_driver_inflight[drv_idx]--;
    const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    if (elapsed_ms > (int64_t)CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS)
    {
        ESP_LOGE(TAG, "Driver '%s' tardo %lld ms en '%s' (watchdog %d ms): "
                      "posible conexion colgada, revisar transporte",
                 drv->profile_id, (long long)elapsed_ms, alias,
                 (int)CONFIG_ROBOT_HAL_EXEC_WATCHDOG_MS);
    }
    robot_hal_set_device_presence(alias, xerr == ESP_OK && out->code == ROBOT_RESULT_OK);
    return xerr;
}

robot_action_id_t robot_action_from_string(const char *s)
{
    if (!s)
    {
        return ROBOT_ACTION_NONE;
    }
    /* Car */
    if (strcasecmp(s, "FORWARD") == 0) return ROBOT_ACTION_FORWARD;
    if (strcasecmp(s, "BACKWARD") == 0) return ROBOT_ACTION_BACKWARD;
    if (strcasecmp(s, "LEFT") == 0) return ROBOT_ACTION_LEFT;
    if (strcasecmp(s, "RIGHT") == 0) return ROBOT_ACTION_RIGHT;
    if (strcasecmp(s, "STOP") == 0) return ROBOT_ACTION_STOP;
    if (strcasecmp(s, "ROTATE") == 0) return ROBOT_ACTION_ROTATE;
    if (strcasecmp(s, "READ_ULTRASONIC") == 0) return ROBOT_ACTION_READ_ULTRASONIC;
    if (strcasecmp(s, "READ_LINE_SENSOR") == 0) return ROBOT_ACTION_READ_LINE_SENSOR;
    if (strcasecmp(s, "READ_BATTERY") == 0) return ROBOT_ACTION_READ_BATTERY;
    /* Arm */
    if (strcasecmp(s, "GRAB") == 0) return ROBOT_ACTION_GRAB;
    if (strcasecmp(s, "RELEASE") == 0) return ROBOT_ACTION_RELEASE;
    if (strcasecmp(s, "ARM_UP") == 0) return ROBOT_ACTION_ARM_UP;
    if (strcasecmp(s, "ARM_DOWN") == 0) return ROBOT_ACTION_ARM_DOWN;
    if (strcasecmp(s, "ARM_HOME") == 0) return ROBOT_ACTION_ARM_HOME;
    if (strcasecmp(s, "MOVE_AXIS") == 0) return ROBOT_ACTION_MOVE_AXIS;
    /* Pan-Tilt */
    if (strcasecmp(s, "PAN_LEFT") == 0) return ROBOT_ACTION_PAN_LEFT;
    if (strcasecmp(s, "PAN_RIGHT") == 0) return ROBOT_ACTION_PAN_RIGHT;
    if (strcasecmp(s, "TILT_UP") == 0) return ROBOT_ACTION_TILT_UP;
    if (strcasecmp(s, "TILT_DOWN") == 0) return ROBOT_ACTION_TILT_DOWN;
    if (strcasecmp(s, "CENTER") == 0) return ROBOT_ACTION_CENTER;
    /* IR */
    if (strcasecmp(s, "SEND_IR_COMMAND") == 0) return ROBOT_ACTION_SEND_IR_COMMAND;
    if (strcasecmp(s, "LEARN_IR_CODE") == 0) return ROBOT_ACTION_LEARN_IR_CODE;
    /* Light */
    if (strcasecmp(s, "TURN_ON") == 0) return ROBOT_ACTION_TURN_ON;
    if (strcasecmp(s, "TURN_OFF") == 0) return ROBOT_ACTION_TURN_OFF;
    if (strcasecmp(s, "TOGGLE") == 0) return ROBOT_ACTION_TOGGLE;
    if (strcasecmp(s, "SET_BRIGHTNESS") == 0) return ROBOT_ACTION_SET_BRIGHTNESS;
    /* ELEGOO-specific strings stay legacy-only until Phase 2 driver extraction */
    return ROBOT_ACTION_NONE;
}

const char *robot_action_to_string(robot_action_id_t action)
{
    switch (action)
    {
    case ROBOT_ACTION_FORWARD: return "FORWARD";
    case ROBOT_ACTION_BACKWARD: return "BACKWARD";
    case ROBOT_ACTION_LEFT: return "LEFT";
    case ROBOT_ACTION_RIGHT: return "RIGHT";
    case ROBOT_ACTION_STOP: return "STOP";
    case ROBOT_ACTION_ROTATE: return "ROTATE";
    case ROBOT_ACTION_READ_ULTRASONIC: return "READ_ULTRASONIC";
    case ROBOT_ACTION_READ_LINE_SENSOR: return "READ_LINE_SENSOR";
    case ROBOT_ACTION_READ_BATTERY: return "READ_BATTERY";
    case ROBOT_ACTION_GRAB: return "GRAB";
    case ROBOT_ACTION_RELEASE: return "RELEASE";
    case ROBOT_ACTION_ARM_UP: return "ARM_UP";
    case ROBOT_ACTION_ARM_DOWN: return "ARM_DOWN";
    case ROBOT_ACTION_ARM_HOME: return "ARM_HOME";
    case ROBOT_ACTION_MOVE_AXIS: return "MOVE_AXIS";
    case ROBOT_ACTION_PAN_LEFT: return "PAN_LEFT";
    case ROBOT_ACTION_PAN_RIGHT: return "PAN_RIGHT";
    case ROBOT_ACTION_TILT_UP: return "TILT_UP";
    case ROBOT_ACTION_TILT_DOWN: return "TILT_DOWN";
    case ROBOT_ACTION_CENTER: return "CENTER";
    case ROBOT_ACTION_SEND_IR_COMMAND: return "SEND_IR_COMMAND";
    case ROBOT_ACTION_LEARN_IR_CODE: return "LEARN_IR_CODE";
    case ROBOT_ACTION_TURN_ON: return "TURN_ON";
    case ROBOT_ACTION_TURN_OFF: return "TURN_OFF";
    case ROBOT_ACTION_TOGGLE: return "TOGGLE";
    case ROBOT_ACTION_SET_BRIGHTNESS: return "SET_BRIGHTNESS";
    default: return "NONE";
    }
}
