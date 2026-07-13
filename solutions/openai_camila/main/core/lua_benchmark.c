#include "lua_benchmark.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "LUA_BENCH";

// Event Group used to block app_main
static EventGroupHandle_t s_bench_done_event = NULL;
#define BENCHMARK_DONE_BIT BIT0

// The strictly controlled PSRAM allocator
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// The isolated benchmark task
static void lua_memory_benchmark_task(void *arg) {
    ESP_LOGI(TAG, "=== STARTING STRICT LUA MEMORY BENCHMARK ===");

    uint32_t base_sram, base_psram;
    uint32_t current_sram, current_psram;

    // --- TEST A: Standard Allocator ---
    ESP_LOGI(TAG, "--- TEST A: Standard Allocator (luaL_newstate) ---");
    base_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    base_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Baseline - SRAM: %lu KB, PSRAM: %lu KB", base_sram / 1024, base_psram / 1024);

    lua_State *L_std = luaL_newstate();
    if (L_std == NULL) {
        ESP_LOGE(TAG, "VALIDATED EXPECTED RESULT: luaL_newstate() failed due to internal SRAM fragmentation/OOM.");
    } else {
        current_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        current_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "L_std Created - SRAM: %lu KB (Delta: -%lu KB), PSRAM: %lu KB (Delta: -%lu KB)",
                 current_sram / 1024, (base_sram - current_sram) / 1024,
                 current_psram / 1024, (base_psram >= current_psram ? (base_psram - current_psram) / 1024 : 0));
        lua_close(L_std);
    }

    ESP_LOGI(TAG, "Delaying 100ms for heap stabilization/defragmentation...");
    vTaskDelay(pdMS_TO_TICKS(100));

    // --- TEST B: PSRAM Allocator ---
    ESP_LOGI(TAG, "--- TEST B: PSRAM Allocator (lua_newstate) ---");
    base_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    base_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Baseline - SRAM: %lu KB, PSRAM: %lu KB", base_sram / 1024, base_psram / 1024);

    lua_State *L_psram = lua_newstate(lua_psram_alloc, NULL, 0);
    if (L_psram == NULL) {
        ESP_LOGE(TAG, "FATAL: lua_newstate() failed to allocate from PSRAM!");
    } else {
        // Load base libraries safely
        luaL_requiref(L_psram, LUA_GNAME, luaopen_base, 1);
        lua_pop(L_psram, 1);
        luaL_requiref(L_psram, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L_psram, 1);
        luaL_requiref(L_psram, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L_psram, 1);

        current_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        current_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "L_psram Created & Loaded - SRAM: %lu KB (Delta: -%lu KB), PSRAM: %lu KB (Delta: -%lu KB)",
                 current_sram / 1024, (base_sram >= current_sram ? (base_sram - current_sram) / 1024 : 0),
                 current_psram / 1024, (base_psram - current_psram) / 1024);
        
        lua_close(L_psram);

        current_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        current_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "L_psram Closed - SRAM: %lu KB, PSRAM: %lu KB", current_sram / 1024, current_psram / 1024);
    }

    ESP_LOGI(TAG, "=== BENCHMARK COMPLETE ===");

    // Signal app_main to continue
    if (s_bench_done_event != NULL) {
        xEventGroupSetBits(s_bench_done_event, BENCHMARK_DONE_BIT);
    }

    // Automatically clean up this task
    vTaskDelete(NULL);
}

void lua_benchmark_run_and_wait(void) {
    s_bench_done_event = xEventGroupCreate();
    if (s_bench_done_event == NULL) {
        ESP_LOGE(TAG, "Failed to create EventGroup for Benchmark. Skipping.");
        return;
    }

    // Use xTaskCreatePinnedToCoreWithCaps to place the TCB and 16KB stack natively in PSRAM
    BaseType_t res = xTaskCreatePinnedToCoreWithCaps(
        lua_memory_benchmark_task,
        "lua_bench",
        16384,
        NULL,
        5, // High priority to run immediately
        NULL,
        1, // Core 1
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create benchmark task in PSRAM!");
        vEventGroupDelete(s_bench_done_event);
        s_bench_done_event = NULL;
        return;
    }

    // Block the boot sequence until the benchmark completes
    ESP_LOGW(TAG, "app_main halted. Waiting for Lua benchmark to complete...");
    xEventGroupWaitBits(s_bench_done_event, BENCHMARK_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    ESP_LOGW(TAG, "Lua benchmark completed. Resuming boot sequence.");

    vEventGroupDelete(s_bench_done_event);
    s_bench_done_event = NULL;
}
