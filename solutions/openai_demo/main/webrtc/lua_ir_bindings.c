#include "lua_ir_bindings.h"
#include "hardware/ir_sniffer.h"
#include "hardware/radar.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "LUA_IR";

extern bool hardware_is_sensor_dock_connected(void);

static int l_ir_start_learning(lua_State *L) {
    ESP_LOGI("ESP_CLAW_ISO", "LUA binding: Arming IR learning mode");
    ir_sniffer_start_learning();
    return 0;
}

static int l_ir_send(lua_State *L) {
    ESP_LOGI("ESP_CLAW_ISO", "LUA binding entered");
    const char *hex_str = luaL_checkstring(L, 1);
    uint32_t hex = (uint32_t)strtoul(hex_str, NULL, 16);
    esp_err_t err = ir_transmitter_send_raw(hex);
    ESP_LOGI("ESP_CLAW_ISO", "LUA binding exit: %s", esp_err_to_name(err));
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

static int l_ir_read_db(lua_State *L) {
    FILE *f = fopen("/littlefs/ir_db.json", "r");
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(f);
        lua_pushnil(L);
        return 1;
    }
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        lua_pushnil(L);
        return 1;
    }
    
    size_t read_bytes = fread(buf, 1, size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    
    lua_pushstring(L, buf);
    free(buf);
    
    return 1;
}

static int l_ir_write_db(lua_State *L) {
    const char *content = luaL_checkstring(L, 1);
    FILE *f = fopen("/littlefs/ir_db.json", "w");
    if (!f) {
        lua_pushboolean(L, false);
        return 1;
    }
    
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fsync(fileno(f));
    fclose(f);
    
    lua_pushboolean(L, written == len);
    return 1;
}

int luaopen_ir(lua_State *L) {
    // Create the empty module table safely
    lua_newtable(L); 
    
    // Map the send and learning functions
    lua_pushcfunction(L, l_ir_send);
    lua_setfield(L, -2, "send");
    
    lua_pushcfunction(L, l_ir_start_learning);
    lua_setfield(L, -2, "start_learning");

    lua_pushcfunction(L, l_ir_read_db);
    lua_setfield(L, -2, "read_db");
    
    lua_pushcfunction(L, l_ir_write_db);
    lua_setfield(L, -2, "write_db");

    return 1;
}
