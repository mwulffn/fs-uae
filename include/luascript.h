/*
* UAE - The Un*x Amiga Emulator
*
* LUA Scripting Layer
*
* Copyright 2013 Frode SOlheim
*/

#ifndef UAE_LUASCRIPT_H
#define UAE_LUASCRIPT_H

#ifdef WITH_LUA
#include <lauxlib.h>

void uae_lua_init(void);
void uae_lua_load(const TCHAR *filename);
void uae_lua_loadall(void);
void uae_lua_free(void);
void uae_lua_init_state(lua_State *L);
void uae_lua_run_handler(const char *name);
#ifdef FSUAE
void uae_lua_service(void);
void uae_lua_memwatch(int num, uaecptr addr, int rwi, int size, uae_u32 *valp);
#endif
void uae_lua_aquire_lock();
void uae_lua_release_lock();

#endif /* WITH_LUA */

#endif /* UAE_LUASCRIPT_H */
