#include "lua_ir_bindings.h"
#include "hardware/ir_sniffer.h"
#include "hardware/radar.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "LUA_IR";

extern uint32_t lookup_ir_code(const char* device, const char* action);
extern bool hardware_is_sensor_dock_connected(void);

static int l_ir_send(lua_State *L) {
    ESP_LOGI("ESP_CLAW_ISO", "LUA binding entered");
    uint32_t hex = (uint32_t)luaL_checkinteger(L, 1);
    esp_err_t err = ir_transmitter_send_raw(hex);
    ESP_LOGI("ESP_CLAW_ISO", "LUA binding exit: %s", esp_err_to_name(err));
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

int luaopen_ir(lua_State *L) {
    // Create the empty module table safely
    lua_newtable(L); 
    
    // Map ONLY the send function
    lua_pushcfunction(L, l_ir_send);
    lua_setfield(L, -2, "send");

    return 1;
}
