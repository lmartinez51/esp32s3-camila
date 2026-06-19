#include "ir_action_map.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "IR_MAP";
static const char *NVS_NAMESPACE = "ir_store";

typedef struct {
    uint32_t codes[IR_MAX_CODES_PER_ACTION];
} ir_action_cache_t;

static ir_action_cache_t s_action_cache[IR_ACTION_MAX];
static SemaphoreHandle_t s_cache_mutex = NULL;

// Helper to convert enum to NVS key string (max 15 characters per NVS limit)
static const char* get_action_key(ir_action_t action) {
    switch (action) {
        case IR_ACTION_MUTE:         return "act_mute";
        case IR_ACTION_UNMUTE:       return "act_unmte";
        case IR_ACTION_TOGGLE_MUTE:  return "act_tg_mte";
        case IR_ACTION_WAKE:         return "act_wake";
        case IR_ACTION_SLEEP:        return "act_slp";
        case IR_ACTION_TOGGLE_SLEEP: return "act_tg_slp";
        default: return NULL;
    }
}

esp_err_t ir_action_map_init(void) {
    esp_err_t err;
    
    if (s_cache_mutex == NULL) {
        s_cache_mutex = xSemaphoreCreateMutex();
        if (!s_cache_mutex) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    memset(&s_action_cache, 0, sizeof(s_action_cache));

    nvs_handle_t nvs_handle;
    // Open the isolated IR namespace
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "IR Action Map namespace '%s' not found. Starting fresh.", NVS_NAMESPACE);
        xSemaphoreGive(s_cache_mutex);
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open IR NVS namespace: %s", esp_err_to_name(err));
        xSemaphoreGive(s_cache_mutex);
        return err;
    }

    // Load arrays into RAM cache
    for (int i = 1; i < IR_ACTION_MAX; i++) {
        const char *key = get_action_key((ir_action_t)i);
        if (!key) continue;

        size_t required_size = sizeof(uint32_t) * IR_MAX_CODES_PER_ACTION;
        err = nvs_get_blob(nvs_handle, key, s_action_cache[i].codes, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded %d bytes for action ID %d", required_size, i);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Error loading blob for action %d: %s", i, esp_err_to_name(err));
        }
    }

    nvs_close(nvs_handle);
    xSemaphoreGive(s_cache_mutex);
    
    ESP_LOGI(TAG, "IR Action Map Cache Initialized successfully");
    return ESP_OK;
}

// Internal helper: Save an action array from RAM to NVS. Assumes mutex is held.
static esp_err_t save_action_to_nvs(ir_action_t action) {
    const char *key = get_action_key(action);
    if (!key) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(nvs_handle, key, s_action_cache[action].codes, sizeof(uint32_t) * IR_MAX_CODES_PER_ACTION);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t ir_map_add_code(ir_action_t target_action, uint32_t hex_code) {
    if (target_action <= IR_ACTION_NONE || target_action >= IR_ACTION_MAX || !s_cache_mutex) return ESP_ERR_INVALID_ARG;
    if (hex_code == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    bool nvs_write_needed[IR_ACTION_MAX] = {false};

    // Step 1: Enforce uniqueness. Remove this code from ALL actions.
    for (int act = 1; act < IR_ACTION_MAX; act++) {
        for (int i = 0; i < IR_MAX_CODES_PER_ACTION; i++) {
            if (s_action_cache[act].codes[i] == hex_code) {
                s_action_cache[act].codes[i] = 0;
                nvs_write_needed[act] = true;
            }
        }
    }

    // Step 2: Add code to the target_action
    bool added = false;
    for (int i = 0; i < IR_MAX_CODES_PER_ACTION; i++) {
        if (s_action_cache[target_action].codes[i] == 0) {
            s_action_cache[target_action].codes[i] = hex_code;
            nvs_write_needed[target_action] = true;
            added = true;
            break;
        }
    }

    if (!added) {
        // Shift array (FIFO) if full
        for (int i = 0; i < IR_MAX_CODES_PER_ACTION - 1; i++) {
            s_action_cache[target_action].codes[i] = s_action_cache[target_action].codes[i + 1];
        }
        s_action_cache[target_action].codes[IR_MAX_CODES_PER_ACTION - 1] = hex_code;
        nvs_write_needed[target_action] = true;
    }

    // Step 3: Write modified arrays to NVS
    esp_err_t last_err = ESP_OK;
    for (int act = 1; act < IR_ACTION_MAX; act++) {
        if (nvs_write_needed[act]) {
            esp_err_t err = save_action_to_nvs((ir_action_t)act);
            if (err != ESP_OK) last_err = err;
        }
    }

    xSemaphoreGive(s_cache_mutex);
    return last_err;
}

esp_err_t ir_map_remove_code(uint32_t hex_code) {
    if (!s_cache_mutex || hex_code == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    esp_err_t last_err = ESP_OK;

    for (int act = 1; act < IR_ACTION_MAX; act++) {
        bool modified = false;
        for (int i = 0; i < IR_MAX_CODES_PER_ACTION; i++) {
            if (s_action_cache[act].codes[i] == hex_code) {
                s_action_cache[act].codes[i] = 0;
                modified = true;
            }
        }
        if (modified) {
            esp_err_t err = save_action_to_nvs((ir_action_t)act);
            if (err != ESP_OK) last_err = err;
        }
    }

    xSemaphoreGive(s_cache_mutex);
    return last_err;
}

ir_action_t ir_map_lookup(uint32_t hex_code) {
    if (!s_cache_mutex || hex_code == 0) return IR_ACTION_NONE;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    ir_action_t found = IR_ACTION_NONE;

    for (int act = 1; act < IR_ACTION_MAX; act++) {
        for (int i = 0; i < IR_MAX_CODES_PER_ACTION; i++) {
            if (s_action_cache[act].codes[i] == hex_code) {
                found = (ir_action_t)act;
                break;
            }
        }
        if (found != IR_ACTION_NONE) break;
    }

    xSemaphoreGive(s_cache_mutex);
    return found;
}
