#include "lua_mic_bindings.h"
#include "app_events.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "LUA_MIC";

static int l_mic_set_mute(lua_State *L) {
    ESP_LOGI(TAG, "LUA mic.set_mute called");
    
    // Check if the argument is a boolean
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    bool mute = lua_toboolean(L, 1);
    
    // Call the orchestrator function
    orchestrator_post_mute_state(mute);
    
    ESP_LOGI(TAG, "LUA mic.set_mute exit: posted mute event: %d", mute);
    
    // Push success result back to Lua
    lua_pushboolean(L, true);
    return 1;
}

int luaopen_mic(lua_State *L) {
    // Create the empty module table safely
    lua_newtable(L); 
    
    // Map the set_mute function
    lua_pushcfunction(L, l_mic_set_mute);
    lua_setfield(L, -2, "set_mute");

    return 1;
}
