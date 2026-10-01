// Lua functions for reading and writing Amiga memory (the mem table).
//
// read_* and write_* go through the memory banks like the CPU does, so
// they have the same side effects as the CPU reading or writing a hardware
// register. peek_* and poke_* access RAM and ROM directly, and fail for
// other addresses.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "debug.h"
#include "identify.h"
#include "memory.h"
#include "options.h"

// Reading megabytes at a time is fine, this only catches mistakes.
#define MAX_RANGE_SIZE (64 * 1024 * 1024)

static uaecptr check_address(lua_State *L, int arg)
{
    lua_Integer addr = luaL_checkinteger(L, arg);
    luaL_argcheck(L, addr >= 0 && addr <= 0xffffffffLL, arg, "address out of range");
    return (uaecptr) addr;
}

// Returns a host pointer to size bytes of RAM or ROM, or NULL.
static uae_u8 *direct_pointer(uaecptr addr, int size)
{
    if (!debug_safe_addr(addr, size)) {
        return NULL;
    }
    return get_real_address(addr);
}

static uae_u8 *check_direct_pointer(lua_State *L, uaecptr addr, int size)
{
    uae_u8 *p = direct_pointer(addr, size);
    if (p == NULL) {
        luaL_error(L, "address %p is not RAM or ROM", (void *) (uintptr_t) addr);
    }
    return p;
}

static int l_read_u8(lua_State *L)
{
    lua_pushinteger(L, get_byte(check_address(L, 1)) & 0xff);
    return 1;
}

static int l_read_u16(lua_State *L)
{
    lua_pushinteger(L, get_word(check_address(L, 1)) & 0xffff);
    return 1;
}

static int l_read_u32(lua_State *L)
{
    lua_pushinteger(L, get_long(check_address(L, 1)));
    return 1;
}

static int l_write_u8(lua_State *L)
{
    put_byte(check_address(L, 1), (uae_u32) luaL_checkinteger(L, 2) & 0xff);
    return 0;
}

static int l_write_u16(lua_State *L)
{
    put_word(check_address(L, 1), (uae_u32) luaL_checkinteger(L, 2) & 0xffff);
    return 0;
}

static int l_write_u32(lua_State *L)
{
    put_long(check_address(L, 1), (uae_u32) luaL_checkinteger(L, 2));
    return 0;
}

static int l_peek_u8(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 1);
    lua_pushinteger(L, p[0]);
    return 1;
}

static int l_peek_u16(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 2);
    lua_pushinteger(L, (p[0] << 8) | p[1]);
    return 1;
}

static int l_peek_u32(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 4);
    lua_pushinteger(L, ((lua_Integer) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    return 1;
}

static int l_poke_u8(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 1);
    p[0] = (uae_u8) luaL_checkinteger(L, 2);
    return 0;
}

static int l_poke_u16(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 2);
    lua_Integer value = luaL_checkinteger(L, 2);
    p[0] = (uae_u8) (value >> 8);
    p[1] = (uae_u8) value;
    return 0;
}

static int l_poke_u32(lua_State *L)
{
    uae_u8 *p = check_direct_pointer(L, check_address(L, 1), 4);
    lua_Integer value = luaL_checkinteger(L, 2);
    p[0] = (uae_u8) (value >> 24);
    p[1] = (uae_u8) (value >> 16);
    p[2] = (uae_u8) (value >> 8);
    p[3] = (uae_u8) value;
    return 0;
}

// read_range(address, length) returns the bytes as a string. Bytes which
// are not RAM or ROM are returned as 0.
static int l_read_range(lua_State *L)
{
    uaecptr addr = check_address(L, 1);
    lua_Integer length = luaL_checkinteger(L, 2);
    luaL_argcheck(L, length >= 0 && length <= MAX_RANGE_SIZE, 2, "invalid length");
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, (size_t) length);
    for (lua_Integer i = 0; i < length; i++) {
        uae_u8 *p = direct_pointer(addr + (uaecptr) i, 1);
        out[i] = p ? (char) *p : 0;
    }
    luaL_pushresultsize(&b, (size_t) length);
    return 1;
}

// write_range(address, string) writes the bytes of the string to RAM or
// ROM.
static int l_write_range(lua_State *L)
{
    uaecptr addr = check_address(L, 1);
    size_t length;
    const char *data = luaL_checklstring(L, 2, &length);
    for (size_t i = 0; i < length; i++) {
        *check_direct_pointer(L, addr + (uaecptr) i, 1) = (uae_u8) data[i];
    }
    return 0;
}

static const luaL_Reg mem_functions[] = {
    {"peek_u16", l_peek_u16},
    {"peek_u32", l_peek_u32},
    {"peek_u8", l_peek_u8},
    {"poke_u16", l_poke_u16},
    {"poke_u32", l_poke_u32},
    {"poke_u8", l_poke_u8},
    {"read_range", l_read_range},
    {"read_u16", l_read_u16},
    {"read_u32", l_read_u32},
    {"read_u8", l_read_u8},
    {"write_range", l_write_range},
    {"write_u16", l_write_u16},
    {"write_u32", l_write_u32},
    {"write_u8", l_write_u8},
    {NULL, NULL},
};

void luaengine_open_mem(lua_State *L)
{
    luaL_newlib(L, mem_functions);
    // mem.custom.COLOR00 and so on are the addresses of the custom chip
    // registers.
    lua_newtable(L);
    for (int i = 0; custd[i].name; i++) {
        lua_pushinteger(L, custd[i].adr);
        lua_setfield(L, -2, custd[i].name);
    }
    lua_setfield(L, -2, "custom");
    lua_setglobal(L, "mem");
}

#endif  // WITH_LUA
