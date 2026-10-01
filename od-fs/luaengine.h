#ifndef UAE_LUAENGINE_H
#define UAE_LUAENGINE_H

#ifdef WITH_LUA

#include <lua.hpp>

// The Lua state shared by startup scripts (lua=<file>), or NULL when Lua
// is not running. Only to be used from the emulation thread.
extern lua_State *g_luaengine_state;

// Logs the error message on top of the stack and pops it.
void luaengine_log_error(lua_State *L, const char *context);

#endif  // WITH_LUA

#endif  // UAE_LUAENGINE_H
