#include "esp_claw_init.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"


#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "esp_littlefs.h"
#include "lua_ir_bindings.h"
#include "lua_mic_bindings.h"
#include "app_events.h"

static const char *TAG = "ESP_CLAW_ISO";
static QueueHandle_t s_test_queue = NULL;
static QueueHandle_t s_lua_to_c_queue = NULL;
static EventGroupHandle_t s_claw_event_group = NULL;

#define LUA_SAFE_TO_START_BIT BIT0
#define LUA_ENGINE_READY_BIT BIT1

bool esp_claw_is_automation_ready(void) {
    if (!s_claw_event_group) return false;
    return (xEventGroupGetBits(s_claw_event_group) & LUA_ENGINE_READY_BIT) != 0;
}

void esp_claw_signal_safe_to_start(void) {
    if (s_claw_event_group) {
        xEventGroupSetBits(s_claw_event_group, LUA_SAFE_TO_START_BIT);
    }
}

static void* claw_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;  (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    } else {
        return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

static void claw_push_rule_to_lua(lua_State *L, esp_claw_rule_t *rule) {
    // Create the main rule table
    lua_newtable(L);
    
    // Set call_id field
    lua_pushstring(L, rule->call_id);
    lua_setfield(L, -2, "call_id");
    
    // Set trigger field
    lua_pushstring(L, rule->trigger);
    lua_setfield(L, -2, "trigger");
    
    // Create conditions sub-table
    lua_newtable(L);
    for (int i = 0; i < rule->num_conditions; i++) {
        lua_newtable(L); // Condition object
        
        lua_pushstring(L, rule->conditions[i].sensor);
        lua_setfield(L, -2, "sensor");
        
        lua_pushstring(L, rule->conditions[i].op);
        lua_setfield(L, -2, "op");
        
        if (rule->conditions[i].val_type == VAL_TYPE_NUMBER) {
            lua_pushnumber(L, rule->conditions[i].f_val);
        } else if (rule->conditions[i].val_type == VAL_TYPE_BOOL) {
            lua_pushboolean(L, rule->conditions[i].b_val);
        } else {
            lua_pushstring(L, rule->conditions[i].s_val);
        }
        lua_setfield(L, -2, "val");
        
        // Arrays in Lua are 1-indexed
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "conditions");
    
    // Create actions sub-table
    lua_newtable(L);
    for (int i = 0; i < rule->num_actions; i++) {
        lua_pushstring(L, rule->actions[i].target);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "actions");
}

static int l_sys_delay(lua_State *L) {
    if (lua_gettop(L) >= 1) {
        int ms = lua_tointeger(L, 1);
        if (ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
    }
    return 0;
}

static int l_send_response(lua_State *L) {
    if (lua_gettop(L) >= 1) {
        const char* msg = lua_tostring(L, 1);
        if (msg && s_lua_to_c_queue) {
            esp_claw_response_t resp;
            resp.success = true;
            strlcpy(resp.payload, msg, sizeof(resp.payload));
            xQueueSend(s_lua_to_c_queue, &resp, 0);
        }
    }
    return 0;
}

extern int send_function_output(const char *call_id, const char *output);
extern int sendEvent(const char *type, const char *payload);

static int l_send_webrtc_response(lua_State *L) {
    if (lua_gettop(L) >= 2) {
        const char* call_id = lua_tostring(L, 1);
        const char* payload = lua_tostring(L, 2);
        if (call_id && payload) {
            // Thread-Safety: send_function_output encapsulates webrtc_send_json
            // which safely takes g_webrtc_mutex (Core 0/1 sync) before transmission.
            send_function_output(call_id, payload);
            sendEvent("response.create", NULL);
        }
    }
    return 0;
}

static int l_inject_webrtc_message(lua_State *L) {
    if (lua_gettop(L) >= 1) {
        const char* message = lua_tostring(L, 1);
        if (message) {
            sendEvent("conversation.item.create", message);
            sendEvent("response.create", NULL);
        }
    }
    return 0;
}

static void* cjson_spiram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void cjson_spiram_free(void* ptr) { heap_caps_free(ptr); }

static int l_save_rules_to_fs(lua_State *L) {
    cJSON_Hooks hooks = { .malloc_fn = cjson_spiram_malloc, .free_fn = cjson_spiram_free };
    cJSON_InitHooks(&hooks);

    cJSON *root_array = cJSON_CreateArray();
    if (!root_array) {
        cJSON_InitHooks(NULL);
        return 0;
    }

    lua_getglobal(L, "rules_db");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            cJSON *rule_json = cJSON_CreateObject();
            
            lua_getfield(L, -1, "trigger");
            cJSON_AddStringToObject(rule_json, "trigger", lua_tostring(L, -1));
            lua_pop(L, 1);
            
            cJSON *conditions_array = cJSON_AddArrayToObject(rule_json, "conditions");
            lua_getfield(L, -1, "conditions");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    cJSON *cond_json = cJSON_CreateObject();
                    lua_getfield(L, -1, "sensor"); cJSON_AddStringToObject(cond_json, "sensor", lua_tostring(L, -1)); lua_pop(L, 1);
                    lua_getfield(L, -1, "op"); cJSON_AddStringToObject(cond_json, "op", lua_tostring(L, -1)); lua_pop(L, 1);
                    lua_getfield(L, -1, "val");
                    if (lua_type(L, -1) == LUA_TNUMBER) {
                        cJSON_AddNumberToObject(cond_json, "val", lua_tonumber(L, -1));
                    } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
                        cJSON_AddBoolToObject(cond_json, "val", lua_toboolean(L, -1));
                    } else {
                        cJSON_AddStringToObject(cond_json, "val", lua_tostring(L, -1));
                    }
                    lua_pop(L, 1);
                    cJSON_AddItemToArray(conditions_array, cond_json);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // pop conditions
            
            cJSON *actions_array = cJSON_AddArrayToObject(rule_json, "actions");
            lua_getfield(L, -1, "actions");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    cJSON_AddItemToArray(actions_array, cJSON_CreateString(lua_tostring(L, -1)));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // pop actions
            
            cJSON_AddItemToArray(root_array, rule_json);
            lua_pop(L, 1); // pop value, keep key for next
        }
    }
    lua_pop(L, 1); // pop rules_db

    char *psram_json_str = cJSON_PrintUnformatted(root_array);
    if (psram_json_str) {
        size_t len = strlen(psram_json_str);
        char *internal_json_buffer = heap_caps_malloc(len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (internal_json_buffer) {
            strcpy(internal_json_buffer, psram_json_str);
            FILE *f = fopen("/littlefs/rules.json.tmp", "w");
            if (f) {
                fputs(internal_json_buffer, f);
                fclose(f);
                rename("/littlefs/rules.json.tmp", "/littlefs/rules.json");
                ESP_LOGI(TAG, "Rules securely saved to LittleFS via Internal SRAM.");
            } else {
                ESP_LOGE(TAG, "Failed to write rules to temporary file.");
            }
            heap_caps_free(internal_json_buffer);
        } else {
            ESP_LOGE(TAG, "Failed to allocate internal RAM for rule save.");
        }
        cjson_spiram_free(psram_json_str);
    }
    
    cJSON_Delete(root_array);
    cJSON_InitHooks(NULL);
    return 0;
}

static void esp_claw_load_rules_from_fs(lua_State *L) {
    FILE *f = fopen("/littlefs/rules.json", "r");
    if (!f) {
        ESP_LOGI(TAG, "No rules.json found (first boot or wiped).");
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return;
    }

    char *json_str = heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM for deserialization.");
        fclose(f);
        return;
    }

    size_t read_bytes = fread(json_str, 1, fsize, f);
    json_str[read_bytes] = '\0';
    fclose(f);

    cJSON_Hooks hooks = { .malloc_fn = cjson_spiram_malloc, .free_fn = cjson_spiram_free };
    cJSON_InitHooks(&hooks);

    cJSON *root = cJSON_Parse(json_str);
    heap_caps_free(json_str);

    if (root && cJSON_IsArray(root)) {
        lua_getglobal(L, "rules_db");
        if (lua_istable(L, -1)) {
            cJSON *rule_item;
            cJSON_ArrayForEach(rule_item, root) {
                cJSON *trigger = cJSON_GetObjectItem(rule_item, "trigger");
                if (!cJSON_IsString(trigger)) continue;

                lua_pushstring(L, trigger->valuestring); // key for rules_db

                lua_newtable(L); // The rule table
                
                lua_pushstring(L, trigger->valuestring);
                lua_setfield(L, -2, "trigger");
                
                cJSON *conditions = cJSON_GetObjectItem(rule_item, "conditions");
                if (cJSON_IsArray(conditions)) {
                    lua_newtable(L);
                    int cond_idx = 1;
                    cJSON *cond_item;
                    cJSON_ArrayForEach(cond_item, conditions) {
                        lua_pushinteger(L, cond_idx++);
                        lua_newtable(L);
                        
                        cJSON *sensor = cJSON_GetObjectItem(cond_item, "sensor");
                        if (cJSON_IsString(sensor)) { lua_pushstring(L, sensor->valuestring); lua_setfield(L, -2, "sensor"); }
                        
                        cJSON *op = cJSON_GetObjectItem(cond_item, "op");
                        if (cJSON_IsString(op)) { lua_pushstring(L, op->valuestring); lua_setfield(L, -2, "op"); }
                        
                        cJSON *val = cJSON_GetObjectItem(cond_item, "val");
                        if (cJSON_IsNumber(val)) { lua_pushnumber(L, val->valuedouble); lua_setfield(L, -2, "val"); }
                        else if (cJSON_IsBool(val)) { lua_pushboolean(L, cJSON_IsTrue(val)); lua_setfield(L, -2, "val"); }
                        else if (cJSON_IsString(val)) { lua_pushstring(L, val->valuestring); lua_setfield(L, -2, "val"); }
                        
                        lua_settable(L, -3); // conditions[cond_idx] = cond_table
                    }
                    lua_setfield(L, -2, "conditions");
                }
                
                cJSON *actions = cJSON_GetObjectItem(rule_item, "actions");
                if (cJSON_IsArray(actions)) {
                    lua_newtable(L);
                    int act_idx = 1;
                    cJSON *act_item;
                    cJSON_ArrayForEach(act_item, actions) {
                        if (cJSON_IsString(act_item)) {
                            lua_pushinteger(L, act_idx++);
                            lua_pushstring(L, act_item->valuestring);
                            lua_settable(L, -3); // actions[act_idx] = act_item
                        }
                    }
                    lua_setfield(L, -2, "actions");
                }
                
                lua_settable(L, -3); // rules_db[trigger] = rule_table
            }
        }
        lua_pop(L, 1); // pop rules_db
        ESP_LOGI(TAG, "Rules securely loaded from LittleFS and injected into Lua rules_db.");
    }
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Failed to parse rules.json.");
        if (root) cJSON_Delete(root);
    }
    cJSON_InitHooks(NULL);
}

static void instruction_limit_hook(lua_State *L, lua_Debug *ar) {
    luaL_error(L, "Execution limit exceeded. Rule Engine killed infinite loop!");
}

static void lua_worker_task(void *arg) {
    ESP_LOGI(TAG, "Starting Lua Isolation Test Worker");
    
    lua_State *L = lua_newstate(claw_lua_alloc, NULL, 0);
    if (!L) {
        ESP_LOGE(TAG, "lua_newstate failed.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Loading approved standard libraries (PSRAM)");
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    
    lua_register(L, "c_sys_delay", l_sys_delay);
    lua_register(L, "c_send_response", l_send_response);
    lua_register(L, "c_send_webrtc_response", l_send_webrtc_response);
    lua_register(L, "c_inject_webrtc_message", l_inject_webrtc_message);
    lua_register(L, "c_save_rules", l_save_rules_to_fs);

    ESP_LOGI(TAG, "Registering IR bindings safely");
    ESP_LOGI("ESP_CLAW_ISO", "HighWater BEFORE=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    luaL_requiref(L, "ir", luaopen_ir, 1);
    lua_pop(L, 1);
    ESP_LOGI("ESP_CLAW_ISO", "IR bindings registered successfully");
    ESP_LOGI(TAG, "Lua initialized. Waiting for LUA_SAFE_TO_START_BIT...");
    xEventGroupWaitBits(s_claw_event_group, LUA_SAFE_TO_START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "LUA_SAFE_TO_START_BIT received. Safe to access SPI Flash.");

    // 1. Initialize LittleFS Partition dynamically
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS");
    } else {
        // 2. Safely load the automation script from LittleFS
        if (luaL_dofile(L, "/littlefs/init.lua") != LUA_OK) {
            ESP_LOGE(TAG, "Failed to load init.lua: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            ESP_LOGI(TAG, "Successfully executed init.lua");
            
            // 3. Load stored dynamic rules
            esp_claw_load_rules_from_fs(L);
            
            // 4. Notify C-System that Lua is online
            xEventGroupSetBits(s_claw_event_group, LUA_ENGINE_READY_BIT);
        }
    }

    ESP_LOGI(TAG, "Entering isolated infinite IPC loop");
    esp_claw_rule_t* msg_rule = NULL;
    while (1) {
        if (xQueueReceive(s_test_queue, &msg_rule, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received new rule pointer. Mapping to Lua table...");
            
            if (msg_rule != NULL) {
                esp_claw_rule_t* current = msg_rule;
                while (current != NULL) {
                    esp_claw_rule_t* next = current->next;
                    
                    // (Hardware Lockdown Interceptor Bypass removed for Phase 1 Rollback)

                    lua_getglobal(L, "register_rule");
                    
                    if (lua_isfunction(L, -1)) {
                        claw_push_rule_to_lua(L, current);
                        
                        lua_sethook(L, instruction_limit_hook, LUA_MASKCOUNT, 50000);
                        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                            ESP_LOGE(TAG, "Lua Execution Error (register_rule): %s", lua_tostring(L, -1));
                            lua_pop(L, 1);
                        }
                        lua_sethook(L, instruction_limit_hook, 0, 0);
                    } else {
                        ESP_LOGE(TAG, "register_rule function not found in Lua environment");
                        lua_pop(L, 1);
                    }
                    
                    free(current);
                    current = next;
                }
            }
            
            lua_gc(L, LUA_GCSTEP, 0);
        }
    }
}

esp_err_t esp_claw_init(void) {
    ESP_LOGI(TAG, "Creating IPC test queue");
    s_test_queue = xQueueCreate(15, sizeof(esp_claw_rule_t*));
    if (!s_test_queue) {
        ESP_LOGE(TAG, "Failed to create s_test_queue");
        return ESP_ERR_NO_MEM;
    }
    
    s_lua_to_c_queue = xQueueCreate(5, sizeof(esp_claw_response_t));
    if (!s_lua_to_c_queue) {
        ESP_LOGE(TAG, "Failed to create s_lua_to_c_queue");
        return ESP_ERR_NO_MEM;
    }

    s_claw_event_group = xEventGroupCreate();
    if (!s_claw_event_group) {
        ESP_LOGE(TAG, "Failed to create s_claw_event_group");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Spawning isolated Lua worker task (Internal SRAM - 16KB)");
    if (xTaskCreatePinnedToCoreWithCaps(lua_worker_task, "lua_worker", 16384, NULL, 3, NULL, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn lua_worker_task.");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_claw_send_command(const char* device, const char* action) {
    if (s_test_queue == NULL) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "esp_claw_send_command is temporarily disabled during JSON Rule Engine migration.");
    return ESP_OK;
}

esp_err_t esp_claw_send_rule(esp_claw_rule_t* rule) {
    if (s_test_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_test_queue, &rule, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_claw_request_list(char* out_buffer, size_t max_len) {
    if (s_test_queue == NULL || s_lua_to_c_queue == NULL) return ESP_ERR_INVALID_STATE;
    
    xQueueReset(s_lua_to_c_queue);

    esp_claw_rule_t* req = malloc(sizeof(esp_claw_rule_t));
    if (!req) return ESP_ERR_NO_MEM;
    
    memset(req, 0, sizeof(esp_claw_rule_t));
    strlcpy(req->trigger, "SYS_CMD:LIST", sizeof(req->trigger));
    
    if (xQueueSend(s_test_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(req);
        strlcpy(out_buffer, "Error: Queue is full.", max_len);
        return ESP_FAIL;
    }
    
    esp_claw_response_t resp;
    if (xQueueReceive(s_lua_to_c_queue, &resp, pdMS_TO_TICKS(1500)) == pdTRUE) {
        snprintf(out_buffer, max_len, "%s", resp.payload);
        return resp.success ? ESP_OK : ESP_FAIL;
    } else {
        strlcpy(out_buffer, "Error: Hardware engine is currently busy executing a macro. Try again in a few seconds.", max_len);
        return ESP_FAIL;
    }
}

esp_err_t esp_claw_request_delete(const char* trigger, char* out_buffer, size_t max_len) {
    if (s_test_queue == NULL || s_lua_to_c_queue == NULL) return ESP_ERR_INVALID_STATE;
    
    xQueueReset(s_lua_to_c_queue);

    esp_claw_rule_t* req = malloc(sizeof(esp_claw_rule_t));
    if (!req) return ESP_ERR_NO_MEM;
    
    memset(req, 0, sizeof(esp_claw_rule_t));
    strlcpy(req->trigger, "SYS_CMD:DELETE", sizeof(req->trigger));
    
    req->num_actions = 1;
    strlcpy(req->actions[0].target, trigger, sizeof(req->actions[0].target));
    req->actions[0].target[sizeof(req->actions[0].target)-1] = '\0';
    
    if (xQueueSend(s_test_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(req);
        strlcpy(out_buffer, "Error: Queue is full.", max_len);
        return ESP_FAIL;
    }
    
    esp_claw_response_t resp;
    if (xQueueReceive(s_lua_to_c_queue, &resp, pdMS_TO_TICKS(1500)) == pdTRUE) {
        snprintf(out_buffer, max_len, "%s", resp.payload);
        return resp.success ? ESP_OK : ESP_FAIL;
    } else {
        strlcpy(out_buffer, "Error: Hardware engine is currently busy executing a macro. Try again in a few seconds.", max_len);
        return ESP_FAIL;
    }
}
