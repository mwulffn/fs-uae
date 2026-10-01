// Lua breakpoints, instruction stepping, exception watches and memory taps
// (the dbg table and mem.tap_*).
//
// Breakpoints do not use the breakpoints of the UAE debugger. While there
// are breakpoints (or instructions left to step), uae_lua_service is called
// before every instruction and calls luaengine_debug_instruction, which
// compares the PC with the breakpoints. Memory taps use the memwatch
// facility in debug.cpp, which calls uae_lua_memwatch.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "debug.h"
#include "events.h"
#include "luascript.h"
#include "memory.h"
#include "newcpu.h"
#include "options.h"

#include <vector>

#define COMMAND_OUTPUT_SIZE (256 * 1024)

struct lua_breakpoint {
    int id;
    uaecptr address;
    // Registry reference to the callback, or LUA_NOREF.
    int callback;
};

struct lua_tap {
    int id;
    // The memwatch node in debug.cpp.
    int node;
    int callback;
};

// A request to stop or call a function when the CPU takes an exception.
struct lua_exception_watch {
    int id;
    // The exception vector number, or one of the values below.
    int vector;
    int callback;
};

// Exceptions which normally mean that the program has crashed.
#define VECTOR_CRASH -1
// The CPU has halted (after a double fault, for example).
#define VECTOR_HALT -2

static std::vector<lua_breakpoint> g_breakpoints;
static std::vector<lua_exception_watch> g_exception_watches;
static std::vector<lua_tap> g_taps;
static int g_next_id = 1;
// When not 0, the number of instructions left to run before stopping.
static int64_t g_step_instructions;
// The PC and time of the last call to luaengine_debug_instruction, to
// detect calls with no instruction run in between.
static uaecptr g_last_pc;
static evt_t g_last_cycles;
// Set while Lua accesses memory, so the access does not run a tap.
static bool g_taps_suspended;

void luaengine_suspend_taps(bool suspend)
{
    g_taps_suspended = suspend;
}

bool luaengine_debug_active(void)
{
    return !g_breakpoints.empty() || g_step_instructions > 0;
}

void luaengine_debug_mark_instruction(void)
{
    g_last_pc = m68k_getpc();
    g_last_cycles = get_cycles();
}

// Runs the callback of a breakpoint. Returns true if it stopped the
// emulation (by calling emu.pause).
static bool run_breakpoint_callback(const lua_breakpoint &breakpoint)
{
    lua_State *L = g_luaengine_state;
    bool was_stopped = luaengine_stop_requested();
    lua_rawgeti(L, LUA_REGISTRYINDEX, breakpoint.callback);
    lua_pushinteger(L, breakpoint.address);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        luaengine_log_error(L, "Error in breakpoint callback");
    }
    return !was_stopped && luaengine_stop_requested();
}

void luaengine_debug_instruction(void)
{
    uaecptr pc = m68k_getpc();
    if (pc == g_last_pc && get_cycles() == g_last_cycles) {
        return;
    }
    luaengine_debug_mark_instruction();
    if (g_step_instructions > 0) {
        g_step_instructions -= 1;
        if (g_step_instructions == 0) {
            luaengine_stop("step", 0, pc);
        }
    }
    // Callbacks can add and remove breakpoints.
    std::vector<lua_breakpoint> breakpoints = g_breakpoints;
    for (const lua_breakpoint &breakpoint : breakpoints) {
        if (breakpoint.address != pc) {
            continue;
        }
        if (breakpoint.callback == LUA_NOREF || run_breakpoint_callback(breakpoint)) {
            luaengine_stop("breakpoint", breakpoint.id, pc);
        }
    }
}

void uae_lua_memwatch(int num, uaecptr addr, int rwi, int size, uae_u32 *valp)
{
    lua_State *L = g_luaengine_state;
    if (L == NULL || g_taps_suspended) {
        return;
    }
    for (size_t i = 0; i < g_taps.size(); i++) {
        if (g_taps[i].node != num) {
            continue;
        }
        int id = g_taps[i].id;
        uae_u32 mask = size == 4 ? 0xffffffff : (1 << (size * 8)) - 1;
        bool was_stopped = luaengine_stop_requested();
        g_taps_suspended = true;
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_taps[i].callback);
        lua_pushinteger(L, addr);
        lua_pushinteger(L, *valp & mask);
        lua_pushinteger(L, size);
        lua_pushinteger(L, regs.instruction_pc);
        if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
            luaengine_log_error(L, "Error in tap callback");
        } else {
            if (lua_isinteger(L, -1)) {
                *valp = (uae_u32) lua_tointeger(L, -1) & mask;
            }
            lua_pop(L, 1);
        }
        g_taps_suspended = false;
        if (!was_stopped && luaengine_stop_requested()) {
            luaengine_stop("tap", id, addr);
        }
        return;
    }
}

static bool is_crash_vector(int nr)
{
    // Bus error, address error, illegal instruction, division by zero and
    // the unimplemented (line A and line F) instructions.
    return nr == 2 || nr == 3 || nr == 4 || nr == 5 || nr == 10 || nr == 11;
}

// Runs the watches matching the vector. reason is "exception" or "halt",
// and number is the exception vector or the halt reason.
static void run_exception_watches(const char *reason, int vector, int number)
{
    lua_State *L = g_luaengine_state;
    if (L == NULL || g_exception_watches.empty()) {
        return;
    }
    // A halted CPU tries the failing instruction again now and then (when
    // the CPU loop is restarted), which is of no interest.
    if (regs.halted && vector != VECTOR_HALT) {
        return;
    }
    uaecptr pc = regs.instruction_pc;
    // Callbacks can add and remove watches.
    std::vector<lua_exception_watch> watches = g_exception_watches;
    for (const lua_exception_watch &watch : watches) {
        bool crash = vector == VECTOR_HALT || is_crash_vector(vector);
        if (watch.vector != vector && !(watch.vector == VECTOR_CRASH && crash)) {
            continue;
        }
        bool stop = watch.callback == LUA_NOREF;
        if (!stop) {
            bool was_stopped = luaengine_stop_requested();
            bool taps_were_suspended = g_taps_suspended;
            g_taps_suspended = true;
            lua_rawgeti(L, LUA_REGISTRYINDEX, watch.callback);
            lua_pushinteger(L, number);
            lua_pushinteger(L, pc);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                luaengine_log_error(L, "Error in exception callback");
            }
            g_taps_suspended = taps_were_suspended;
            stop = !was_stopped && luaengine_stop_requested();
        }
        if (stop) {
            luaengine_stop(reason, watch.id, pc, number);
        }
    }
}

void uae_lua_exception(int nr)
{
    run_exception_watches("exception", nr, nr);
}

void uae_lua_halted(int reason)
{
    run_exception_watches("halt", VECTOR_HALT, reason);
}

// dbg.bpset(address, callback) sets a breakpoint and returns its id.
// Without a callback, the emulation stops when the breakpoint is reached.
// With one, callback(address) is called and the emulation continues, unless
// the callback calls emu.pause.
static int l_dbg_bpset(lua_State *L)
{
    lua_Integer address = luaL_checkinteger(L, 1);
    lua_breakpoint breakpoint;
    breakpoint.id = g_next_id++;
    breakpoint.address = (uaecptr) address;
    breakpoint.callback = LUA_NOREF;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 2);
        breakpoint.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_breakpoints.push_back(breakpoint);
    luaengine_debug_mark_instruction();
    set_special(SPCFLAG_BRK);
    lua_pushinteger(L, breakpoint.id);
    return 1;
}

// dbg.bpclear(id) removes a breakpoint, dbg.bpclear() removes all.
static int l_dbg_bpclear(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_breakpoints.size(); i > 0; i--) {
        if (all || g_breakpoints[i - 1].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_breakpoints[i - 1].callback);
            g_breakpoints.erase(g_breakpoints.begin() + i - 1);
        }
    }
    return 0;
}

// dbg.bplist() returns a list of {id = ..., address = ...}.
static int l_dbg_bplist(lua_State *L)
{
    lua_createtable(L, (int) g_breakpoints.size(), 0);
    for (size_t i = 0; i < g_breakpoints.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, g_breakpoints[i].id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, g_breakpoints[i].address);
        lua_setfield(L, -2, "address");
        lua_rawseti(L, -2, (lua_Integer) i + 1);
    }
    return 1;
}

// dbg.exset(vector, callback) watches for a CPU exception and returns an
// id. vector is the exception vector number (4 is illegal instruction, 32
// is TRAP #0 and so on), "crash" for the exceptions which normally mean that
// the program has crashed (and for the CPU halting), or "halt" for the CPU
// halting only. Without a callback, the emulation stops when the exception
// is taken, at the first instruction of the exception handler. With one,
// callback(vector, pc) is called and the emulation continues, unless the
// callback calls emu.pause. pc is the address of the instruction which
// caused the exception. For a halt, the callback gets the halt reason of
// the UAE core instead of a vector.
static int l_dbg_exset(lua_State *L)
{
    lua_exception_watch watch;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 1);
        if (strcmp(name, "crash") == 0) {
            watch.vector = VECTOR_CRASH;
        } else if (strcmp(name, "halt") == 0) {
            watch.vector = VECTOR_HALT;
        } else {
            return luaL_argerror(L, 1, "must be a vector number, 'crash' or 'halt'");
        }
    } else {
        lua_Integer vector = luaL_checkinteger(L, 1);
        luaL_argcheck(L, vector >= 2 && vector <= 255, 1, "must be 2 to 255");
        watch.vector = (int) vector;
    }
    watch.id = g_next_id++;
    watch.callback = LUA_NOREF;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 2);
        watch.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_exception_watches.push_back(watch);
    lua_pushinteger(L, watch.id);
    return 1;
}

// dbg.exclear(id) removes an exception watch, dbg.exclear() removes all.
static int l_dbg_exclear(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_exception_watches.size(); i > 0; i--) {
        if (all || g_exception_watches[i - 1].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_exception_watches[i - 1].callback);
            g_exception_watches.erase(g_exception_watches.begin() + i - 1);
        }
    }
    return 0;
}

// dbg.go() continues after a stop. This is the same as emu.resume.
static int l_dbg_go(lua_State *L)
{
    luaengine_resume();
    return 0;
}

// dbg.wait(frames) waits until the emulation stops and returns a table
// describing why: {reason = "breakpoint", "tap", "step" or "pause",
// pc = ..., id = ..., address = ...}. If frames is given and the emulation
// has not stopped after that many frames, nothing is returned.
static int l_dbg_wait(lua_State *L)
{
    return luaengine_yield_until_stopped(L, luaL_optinteger(L, 1, 0));
}

// dbg.step(count) runs count instructions (default 1) and stops. Returns
// the same as dbg.wait.
static int l_dbg_step(lua_State *L)
{
    lua_Integer count = luaL_optinteger(L, 1, 1);
    luaL_argcheck(L, count >= 1, 1, "must be at least 1");
    g_step_instructions = count;
    luaengine_debug_mark_instruction();
    set_special(SPCFLAG_BRK);
    luaengine_resume();
    return luaengine_yield_until_stopped(L, 0);
}

// dbg.stopped() returns the same table as dbg.wait if the emulation is
// stopped, and false if it is running.
static int l_dbg_stopped(lua_State *L)
{
    luaengine_push_stop_info(L);
    return 1;
}

// dbg.command(text) runs a command in the UAE debugger and returns its
// output. This is for commands showing information; commands which
// continue the emulation do not work from here.
static int l_dbg_command(lua_State *L)
{
    const char *command = luaL_checkstring(L, 1);
    TCHAR *out = xcalloc(TCHAR, COMMAND_OUTPUT_SIZE);
    g_taps_suspended = true;
    debug_parser(command, out, COMMAND_OUTPUT_SIZE);
    g_taps_suspended = false;
    lua_pushstring(L, out);
    xfree(out);
    return 1;
}

static const luaL_Reg dbg_functions[] = {
    {"bpclear", l_dbg_bpclear},
    {"bplist", l_dbg_bplist},
    {"bpset", l_dbg_bpset},
    {"command", l_dbg_command},
    {"exclear", l_dbg_exclear},
    {"exset", l_dbg_exset},
    {"go", l_dbg_go},
    {"step", l_dbg_step},
    {"stopped", l_dbg_stopped},
    {"wait", l_dbg_wait},
    {NULL, NULL},
};

static int add_tap(lua_State *L, int rwi)
{
    lua_Integer first = luaL_checkinteger(L, 1);
    lua_Integer last = luaL_checkinteger(L, 2);
    luaL_argcheck(L, first >= 0 && first <= 0xffffffffLL, 1, "address out of range");
    luaL_argcheck(L, last >= first && last <= 0xffffffffLL, 2, "address out of range");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_tap tap;
    tap.node = debug_lua_memwatch_add((uaecptr) first, (int) (last - first + 1), rwi);
    if (tap.node < 0) {
        return luaL_error(L, "too many taps");
    }
    tap.id = g_next_id++;
    lua_settop(L, 3);
    tap.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    g_taps.push_back(tap);
    lua_pushinteger(L, tap.id);
    return 1;
}

// mem.tap_read(first, last, callback) calls callback(address, value, size,
// pc) when the CPU reads from the address range. If the callback returns an
// integer, the CPU reads that value instead. Returns the id of the tap.
// The callback is called for each bus access, and the emulated CPU can
// access a long word as two words.
static int l_mem_tap_read(lua_State *L)
{
    return add_tap(L, 1);
}

// mem.tap_write(first, last, callback) is the same for writes. If the
// callback returns an integer, that value is written instead.
static int l_mem_tap_write(lua_State *L)
{
    return add_tap(L, 2);
}

// mem.tap_remove(id) removes a tap, mem.tap_remove() removes all.
static int l_mem_tap_remove(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_taps.size(); i > 0; i--) {
        if (all || g_taps[i - 1].id == id) {
            debug_lua_memwatch_remove(g_taps[i - 1].node);
            luaL_unref(L, LUA_REGISTRYINDEX, g_taps[i - 1].callback);
            g_taps.erase(g_taps.begin() + i - 1);
        }
    }
    return 0;
}

static const luaL_Reg tap_functions[] = {
    {"tap_read", l_mem_tap_read},
    {"tap_remove", l_mem_tap_remove},
    {"tap_write", l_mem_tap_write},
    {NULL, NULL},
};

void luaengine_open_dbg(lua_State *L)
{
    luaL_newlib(L, dbg_functions);
    lua_setglobal(L, "dbg");
    lua_getglobal(L, "mem");
    luaL_setfuncs(L, tap_functions, 0);
    lua_pop(L, 1);
}

void luaengine_debug_free(void)
{
    for (const lua_tap &tap : g_taps) {
        debug_lua_memwatch_remove(tap.node);
    }
    g_taps.clear();
    g_breakpoints.clear();
    g_exception_watches.clear();
    g_step_instructions = 0;
    g_taps_suspended = false;
}

#endif  // WITH_LUA
