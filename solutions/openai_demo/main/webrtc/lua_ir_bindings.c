#include "lua_ir_bindings.h"
#include "hardware/ir_sniffer.h"
#include "hardware/radar.h"
#include "esp_log.h"

static const char *TAG = "LUA_IR";

static int l_ir_send(lua_State *L) {
    // 1. Check for valid integer argument
    uint32_t hex_code = (uint32_t)luaL_checkinteger(L, 1);
    
    // 2. Hardware constraint check
    if (!hardware_is_sensor_dock_connected()) {
        ESP_LOGW(TAG, "IR Send aborted: SENSOR dock is disconnected");
        lua_pushnil(L);
        lua_pushstring(L, "ERR_HARDWARE: SENSOR dock is disconnected");
        return 2;
    }
    
    // 3. Perform transmission
    esp_err_t err = ir_transmitter_send_raw(hex_code);
    if (err == ESP_OK) {
        lua_pushboolean(L, 1); // Success
        return 1;
    } else {
        lua_pushnil(L);
        lua_pushfstring(L, "ERR_RMT_TX: %s", esp_err_to_name(err));
        return 2;
    }
}

static const struct luaL_Reg ir_lib[] = {
    {"send", l_ir_send},
    {NULL, NULL}
};

int luaopen_ir(lua_State *L) {
    luaL_newlib(L, ir_lib);
    return 1;
}
