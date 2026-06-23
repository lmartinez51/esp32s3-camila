#include "esp_claw_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_ir_bindings.h"

static const char *TAG = "ESP_CLAW";
static QueueHandle_t s_lua_script_queue = NULL;

static void* lua_alloc_spiram(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;  (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    } else {
        return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

#include "app_events.h"

static void lua_worker_task(void *arg) {
    // Wait for the explicit True Idle signal from the Orchestrator to prevent PSRAM/SPI collisions
    extern EventGroupHandle_t app_startup_event_group;
    if (app_startup_event_group != NULL) {
        xEventGroupWaitBits(app_startup_event_group, LUA_SAFE_TO_START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    lua_State *L = lua_newstate(lua_alloc_spiram, NULL, 0);
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        vTaskDelete(NULL);
        return;
    }

    // Manual chunked loading to prevent RTOS starvation and heap mutex contention
    static const luaL_Reg loadedlibs[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_LOADLIBNAME, luaopen_package},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_IOLIBNAME, luaopen_io},
        {LUA_OSLIBNAME, luaopen_os},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_DBLIBNAME, luaopen_debug},
        {NULL, NULL}
    };

    for (const luaL_Reg *lib = loadedlibs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
        // Breathe! Yield CPU and release global heap mutex to let Hardware Radar and Orchestrator run
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    luaL_requiref(L, "ir", luaopen_ir, 1);
    lua_pop(L, 1);
    vTaskDelay(pdMS_TO_TICKS(20)); // Final yield

    char* script;
    while(1) {
        if (xQueueReceive(s_lua_script_queue, &script, portMAX_DELAY) == pdTRUE) {
            int ret = luaL_dostring(L, script);
            if (ret != LUA_OK) {
                ESP_LOGE(TAG, "Lua Error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            } else {
                ESP_LOGI(TAG, "Lua Script executed successfully");
            }
            free(script);
        }
    }
}

esp_err_t esp_claw_init(void) {
    s_lua_script_queue = xQueueCreate(10, sizeof(char*));
    if (!s_lua_script_queue) {
        return ESP_ERR_NO_MEM;
    }

    // Increased stack size to 32KB to prevent silent stack overflow freezes during Lua initialization,
    // pinned to Core 1, and explicitly downgraded to tskIDLE_PRIORITY (Priority 0)
    // so it ONLY runs when Wi-Fi, Orchestrator, and hardware sensors are idle.
    if (xTaskCreatePinnedToCore(lua_worker_task, "lua_worker", 32768, NULL, tskIDLE_PRIORITY, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ESP-Claw Lua Engine Initialized.");
    return ESP_OK;
}

esp_err_t esp_claw_execute_script(const char* script) {
    if (!s_lua_script_queue || !script) return ESP_ERR_INVALID_STATE;
    
    char* script_copy = strdup(script);
    if (!script_copy) return ESP_ERR_NO_MEM;
    
    if (xQueueSend(s_lua_script_queue, &script_copy, 0) != pdTRUE) {
        free(script_copy); // Prevent memory leak on queue full
        ESP_LOGW(TAG, "Lua script queue full, dropped.");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
