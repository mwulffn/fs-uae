// Lua scripting engine for FS-UAE.
//
// Implements the interface in include/luascript.h (used by the UAE core),
// replacing the original luascript.cpp. One Lua state is shared by all
// scripts, and everything runs on the emulation thread.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

// Must be included before luascript.h so the Lua API gets C linkage.
#include "luaengine.h"

#include "luascript.h"
#include "options.h"

lua_State *g_luaengine_state;

void luaengine_log_error(lua_State *L, const char *context)
{
    const char *message = lua_tostring(L, -1);
    write_log("[LUA] %s: %s\n", context, message ? message : "(no message)");
    lua_pop(L, 1);
}

// Concatenates the arguments like print does, leaving the result on the
// stack.
static void concat_print_arguments(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
}

static int l_print(lua_State *L)
{
    concat_print_arguments(L);
    write_log("[LUA] %s\n", lua_tostring(L, -1));
    return 0;
}

static int l_emu_log(lua_State *L)
{
    write_log("[LUA] %s\n", luaL_checkstring(L, 1));
    return 0;
}

static const luaL_Reg emu_functions[] = {
    {"log", l_emu_log},
    {NULL, NULL},
};

void uae_lua_init_state(lua_State *L)
{
    luaL_openlibs(L);
    lua_register(L, "print", l_print);
    luaL_newlib(L, emu_functions);
    lua_setglobal(L, "emu");
}

void uae_lua_load(const TCHAR *filename)
{
    lua_State *L = g_luaengine_state;
    if (L == NULL || filename[0] == '\0') {
        return;
    }
    write_log("[LUA] Loading %s\n", filename);
    if (luaL_loadfile(L, filename) != LUA_OK) {
        luaengine_log_error(L, "Load failed");
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        luaengine_log_error(L, filename);
    }
}

void uae_lua_loadall(void)
{
    bool have_scripts = false;
    for (int i = 0; i < MAX_LUA_STATES; i++) {
        if (currprefs.luafiles[i][0]) {
            have_scripts = true;
        }
    }
    if (!have_scripts) {
        return;
    }
    if (g_luaengine_state == NULL) {
        g_luaengine_state = luaL_newstate();
        uae_lua_init_state(g_luaengine_state);
    }
    for (int i = 0; i < MAX_LUA_STATES; i++) {
        uae_lua_load(currprefs.luafiles[i]);
    }
}

void uae_lua_run_handler(const char *name)
{
    lua_State *L = g_luaengine_state;
    if (L == NULL) {
        return;
    }
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        luaengine_log_error(L, name);
    }
}

void uae_lua_init(void)
{
}

void uae_lua_free(void)
{
    if (g_luaengine_state != NULL) {
        lua_close(g_luaengine_state);
        g_luaengine_state = NULL;
    }
}

#endif  // WITH_LUA
