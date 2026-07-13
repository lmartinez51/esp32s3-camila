#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Block the boot sequence to run a strictly isolated Memory Benchmark 
 * for the Lua Engine, verifying the custom PSRAM allocator.
 */
void lua_benchmark_run_and_wait(void);

#ifdef __cplusplus
}
#endif
