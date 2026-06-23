#pragma once

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the 'ir' module into the given Lua state.
 * 
 * @param L The Lua state
 * @return int Number of return values on the Lua stack (always 1: the module table)
 */
int luaopen_ir(lua_State *L);

#ifdef __cplusplus
}
#endif
