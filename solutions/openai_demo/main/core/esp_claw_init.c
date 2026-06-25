#include "esp_claw_init.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include <string.h>
#include "hardware/ir_sniffer.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_ir_bindings.h"

static const char *TAG = "ESP_CLAW_ISO";
static QueueHandle_t s_test_queue = NULL;

static void* claw_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;  (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    } else {
        return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

static void lua_worker_task(void *arg) {
    ESP_LOGI(TAG, "Starting Lua Isolation Test Worker");
    
    lua_State *L = lua_newstate(claw_lua_alloc, NULL, 0);
    if (!L) {
        ESP_LOGE(TAG, "lua_newstate failed.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Loading standard libraries (PSRAM)");
    luaL_openlibs(L);

    ESP_LOGI(TAG, "Registering IR bindings safely");
    ESP_LOGI("ESP_CLAW_ISO", "HighWater BEFORE=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    luaL_requiref(L, "ir", luaopen_ir, 1);
    lua_pop(L, 1);
    ESP_LOGI("ESP_CLAW_ISO", "IR bindings registered successfully");

    const char* lua_core_logic = 
        "function process_command(device, action)\n"
        "    print('Lua routing: ' .. device .. ' -> ' .. action)\n"
        "    local map = {\n"
        "        tv = {\n"
        "            power = 0xFD020707,\n"
        "            vol_up = 0xF8070707,\n"
        "            vol_down = 0xF40B0707,\n"
        "            ch_up = 0xED120707,\n"
        "            ch_down = 0xEF100707,\n"
        "            mute = 0xF00F0707,\n"
        "            num_1 = 0xFB040707,\n"
        "            num_2 = 0xFA050707,\n"
        "            num_3 = 0xF9060707,\n"
        "            num_4 = 0xF7080707,\n"
        "            num_5 = 0xF6090707,\n"
        "            num_6 = 0xF50A0707,\n"
        "            num_7 = 0xF30C0707,\n"
        "            num_8 = 0xF20D0707,\n"
        "            num_9 = 0xF10E0707,\n"
        "            num_0 = 0xEE110707,\n"
        "            dash = 0xDC230707\n"
        "        }\n"
        "    }\n"
        "    local dev_map = map[device]\n"
        "    if dev_map and dev_map[action] then\n"
        "        ir.send(dev_map[action])\n"
        "        print('IR executed correctly by Lua')\n"
        "    else\n"
        "        print('Error: Device or action not found in Lua map')\n"
        "    end\n"
        "end";

    if (luaL_dostring(L, lua_core_logic) != LUA_OK) {
        ESP_LOGE("ESP_CLAW", "Failed to load core logic: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    ESP_LOGI(TAG, "Entering isolated infinite IPC loop");
    esp_claw_msg_t msg;
    while (1) {
        if (xQueueReceive(s_test_queue, &msg, portMAX_DELAY) == pdTRUE) {
            lua_getglobal(L, "process_command");
            lua_pushstring(L, msg.device);
            lua_pushstring(L, msg.action);

            // Call function with 2 arguments and 0 returns
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                ESP_LOGE("ESP_CLAW", "Lua Execution Error: %s", lua_tostring(L, -1));
                lua_pop(L, 1); // Clean error message from stack
            }
        }
    }
}

esp_err_t esp_claw_init(void) {
    ESP_LOGI(TAG, "Creating IPC test queue");
    s_test_queue = xQueueCreate(5, sizeof(esp_claw_msg_t));
    if (!s_test_queue) {
        ESP_LOGE(TAG, "Failed to create s_test_queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Spawning isolated Lua worker task");
    if (xTaskCreatePinnedToCore(lua_worker_task, "lua_worker", 16384, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn lua_worker_task.");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_claw_send_command(const char* device, const char* action) {
    if (s_test_queue == NULL) return ESP_ERR_INVALID_STATE;
    esp_claw_msg_t msg = {0};
    strlcpy(msg.device, device, sizeof(msg.device));
    strlcpy(msg.action, action, sizeof(msg.action));
    // Use a 0 timeout to prevent WebRTC starvation if queue is full
    if (xQueueSend(s_test_queue, &msg, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
