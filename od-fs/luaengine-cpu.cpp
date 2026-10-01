// Lua access to the CPU registers (the cpu table).
//
// The registers are fields: cpu.d0 to cpu.d7, cpu.a0 to cpu.a7, cpu.pc,
// cpu.sr, cpu.usp, cpu.isp, cpu.msp and cpu.vbr.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "memory.h"
#include "newcpu.h"
#include "options.h"

// Returns the index into regs.regs for d0-d7 and a0-a7, or -1.
static int general_register_index(const char *name)
{
    if ((name[0] == 'd' || name[0] == 'a') && name[1] >= '0' && name[1] <= '7' && name[2] == '\0') {
        return (name[0] == 'a' ? 8 : 0) + name[1] - '0';
    }
    return -1;
}

// A7 is the stack pointer selected by SR, the other two are kept in regs.
static uae_u32 *stack_pointer(bool supervisor, bool master)
{
    bool active = regs.s == supervisor && (!supervisor || (regs.m != 0) == master);
    if (active) {
        return &m68k_areg(regs, 7);
    }
    return !supervisor ? &regs.usp : master ? &regs.msp : &regs.isp;
}

static int l_cpu_index(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    int index = general_register_index(name);
    if (index >= 0) {
        lua_pushinteger(L, regs.regs[index]);
    } else if (strcmp(name, "pc") == 0) {
        lua_pushinteger(L, m68k_getpc());
    } else if (strcmp(name, "sr") == 0) {
        MakeSR();
        lua_pushinteger(L, regs.sr);
    } else if (strcmp(name, "usp") == 0) {
        lua_pushinteger(L, *stack_pointer(false, false));
    } else if (strcmp(name, "isp") == 0) {
        lua_pushinteger(L, *stack_pointer(true, false));
    } else if (strcmp(name, "msp") == 0) {
        lua_pushinteger(L, *stack_pointer(true, true));
    } else if (strcmp(name, "vbr") == 0) {
        lua_pushinteger(L, regs.vbr);
    } else {
        return luaL_error(L, "unknown CPU register '%s'", name);
    }
    return 1;
}

static int l_cpu_newindex(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    uae_u32 value = (uae_u32) luaL_checkinteger(L, 3);
    int index = general_register_index(name);
    if (index >= 0) {
        regs.regs[index] = value;
    } else if (strcmp(name, "pc") == 0) {
        m68k_setpc_normal(value);
        fill_prefetch();
    } else if (strcmp(name, "sr") == 0) {
        regs.sr = (uae_u16) value;
        MakeFromSR();
    } else if (strcmp(name, "usp") == 0) {
        *stack_pointer(false, false) = value;
    } else if (strcmp(name, "isp") == 0) {
        *stack_pointer(true, false) = value;
    } else if (strcmp(name, "msp") == 0) {
        *stack_pointer(true, true) = value;
    } else if (strcmp(name, "vbr") == 0) {
        regs.vbr = value;
    } else {
        return luaL_error(L, "unknown CPU register '%s'", name);
    }
    return 0;
}

// cpu.disasm(address, count) returns a list with a table for each
// instruction: {address = ..., size = ..., text = "..."}.
static int l_cpu_disasm(lua_State *L)
{
    uaecptr addr = (uaecptr) luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, count >= 1 && count <= 10000, 2, "invalid count");
    lua_createtable(L, (int) count, 0);
    for (lua_Integer i = 1; i <= count; i++) {
        TCHAR text[MAX_DPATH];
        uaecptr next;
        sm68k_disasm(text, NULL, addr, &next, 0xffffffff);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, addr);
        lua_setfield(L, -2, "address");
        lua_pushinteger(L, next - addr);
        lua_setfield(L, -2, "size");
        lua_pushstring(L, text);
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, i);
        addr = next;
    }
    return 1;
}

void luaengine_open_cpu(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_cpu_disasm);
    lua_setfield(L, -2, "disasm");
    lua_newtable(L);
    lua_pushcfunction(L, l_cpu_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_cpu_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "cpu");
}

#endif  // WITH_LUA
