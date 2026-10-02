// Lua functions for save states (the state table) and floppy disks (the
// media table).

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "disk.h"
#include "newcpu.h"
#include "options.h"
#include "savestate.h"
#include "zfile.h"

// While a directory hard drive is handling a request from the Amiga, the
// UAE core does not save states, and tries again at the end of the next
// frame. This is how many frames state.save and state.snapshot wait.
#define MAX_SAVE_FRAMES 100

// The UAE core saves a state at the end of a frame. With a cycle-exact CPU
// that is in the middle of an instruction, and the state then needs a
// record of what the instruction has done so far, for the instruction to be
// continued when the state is loaded. The CPU tracer of the UAE core makes
// that record. Without it, the instruction is cut in two, and the program
// can crash when the state is loaded.
//
// The tracer must have followed the instruction from its start, so the UAE
// core turns it on a frame before it saves a state for one of its own keys,
// and off again afterwards. The functions here do the same, but only for
// the 68000: the tracer also exists for the 68020, where states saved with
// it crashed the program more often than states saved without it (see issue
// 19 in mwulffn/fs-uae).
//
// Returns true if the tracer was turned on, and the state must not be saved
// before the end of the next frame.
static bool start_cpu_tracer(void)
{
    return currprefs.cpu_model == 68000 && !is_cpu_tracer() && set_cpu_tracer(true);
}

// Returns true if the state has still not been saved because the file
// system is busy, and there are frames left to wait.
static bool save_must_wait(lua_KContext frames)
{
    return savestate_state != 0 && savestate_busy_frames > 0 && frames < MAX_SAVE_FRAMES;
}

static int save_finished(lua_State *L, int status, lua_KContext frames)
{
    if (save_must_wait(frames)) {
        return luaengine_yield_frames(L, 1, save_finished, frames + 1);
    }
    // This leaves the tracer on if something else needs it.
    set_cpu_tracer(false);
    if (savestate_state != 0) {
        // The state is saved when savestate_state is cleared.
        savestate_state = 0;
        return luaL_error(L, "could not save the state");
    }
    return 0;
}

static int save_start(lua_State *L, int status, lua_KContext context)
{
    _tcscpy(savestate_fname, lua_tostring(L, 1));
    savestate_state = STATE_SAVE;
    return luaengine_yield_frames(L, 1, save_finished, 1);
}

// state.save(path) saves a state file. The UAE core saves states at the
// end of a frame, so this returns when the current frame is finished, or
// the one after it if the CPU tracer had to be turned on first.
static int l_state_save(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_argcheck(L, strlen(path) < MAX_DPATH, 1, "path is too long");
    lua_settop(L, 1);
    if (start_cpu_tracer()) {
        return luaengine_yield_frames(L, 1, save_start, 0);
    }
    return save_start(L, LUA_OK, 0);
}

// The number of frames to wait for a state to be loaded.
#define MAX_LOAD_FRAMES 10

static int load_finished(lua_State *L, int status, lua_KContext frames)
{
    if (savestate_state != 0) {
        // The emulation is restarted to load the state, and that can take
        // more than one frame.
        if (frames < MAX_LOAD_FRAMES) {
            return luaengine_yield_frames(L, 1, load_finished, frames + 1);
        }
        savestate_state = 0;
        return luaL_error(L, "could not load the state");
    }
    return 0;
}

// state.load(path) loads a state file. The state is loaded at the end of
// the current frame, and this returns when a frame has been run after that.
static int l_state_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_argcheck(L, strlen(path) < MAX_DPATH, 1, "path is too long");
    if (!zfile_exists(path)) {
        return luaL_error(L, "state file '%s' does not exist", path);
    }
    _tcscpy(savestate_fname, path);
    savestate_state = STATE_DORESTORE;
    return luaengine_yield_frames(L, 1, load_finished, 1);
}

// A state kept in memory, as a Lua userdata.
struct state_snapshot {
    uae_u8 *data;
    size_t size;
};

#define SNAPSHOT_TYPE "fsuae.snapshot"

static int snapshot_gc(lua_State *L)
{
    state_snapshot *snapshot = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    xfree(snapshot->data);
    snapshot->data = NULL;
    return 0;
}

// #snapshot is its size in bytes.
static int snapshot_len(lua_State *L)
{
    state_snapshot *snapshot = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    lua_pushinteger(L, snapshot->size);
    return 1;
}

static int snapshot_finished(lua_State *L, int status, lua_KContext frames)
{
    if (save_must_wait(frames)) {
        return luaengine_yield_frames(L, 1, snapshot_finished, frames + 1);
    }
    set_cpu_tracer(false);
    size_t size;
    uae_u8 *data = savestate_memory_save_result(&size);
    if (data == NULL) {
        savestate_state = 0;
        return luaL_error(L, "could not save the state");
    }
    state_snapshot *snapshot = (state_snapshot *) lua_newuserdatauv(L, sizeof(state_snapshot), 0);
    snapshot->data = data;
    snapshot->size = size;
    luaL_setmetatable(L, SNAPSHOT_TYPE);
    return 1;
}

static int snapshot_start(lua_State *L, int status, lua_KContext context)
{
    savestate_memory_save_request();
    return luaengine_yield_frames(L, 1, snapshot_finished, 1);
}

// state.snapshot() saves a state in memory and returns it. The state is
// saved at the same time as state.save would save it.
static int l_state_snapshot(lua_State *L)
{
    if (start_cpu_tracer()) {
        return luaengine_yield_frames(L, 1, snapshot_start, 0);
    }
    return snapshot_start(L, LUA_OK, 0);
}

// state.restore(snapshot) loads a state returned by state.snapshot. It can
// be loaded any number of times. Like state.load, it returns when a frame
// has been run after loading it.
static int l_state_restore(lua_State *L)
{
    state_snapshot *snapshot = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    savestate_memory_restore_request(snapshot->data, snapshot->size);
    return luaengine_yield_frames(L, 1, load_finished, 1);
}

static const luaL_Reg snapshot_metamethods[] = {
    {"__gc", snapshot_gc},
    {"__len", snapshot_len},
    {NULL, NULL},
};

static const luaL_Reg state_functions[] = {
    {"load", l_state_load},
    {"restore", l_state_restore},
    {"save", l_state_save},
    {"snapshot", l_state_snapshot},
    {NULL, NULL},
};

static int check_drive(lua_State *L, int arg)
{
    lua_Integer drive = luaL_checkinteger(L, arg);
    luaL_argcheck(L, drive >= 0 && drive <= 3, arg, "drive must be 0 to 3");
    return (int) drive;
}

// media.insert(drive, path) inserts a disk image in DF0 to DF3. Like when
// changing disks in a real drive, it takes a moment before the new disk is
// in the drive.
static int l_media_insert(lua_State *L)
{
    int drive = check_drive(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!zfile_exists(path)) {
        return luaL_error(L, "disk image '%s' does not exist", path);
    }
    disk_insert(drive, path);
    return 0;
}

static int l_media_eject(lua_State *L)
{
    disk_eject(check_drive(L, 1));
    return 0;
}

// media.path(drive) returns the path of the disk image in the drive, or an
// empty string.
static int l_media_path(lua_State *L)
{
    lua_pushstring(L, currprefs.floppyslots[check_drive(L, 1)].df);
    return 1;
}

static const luaL_Reg media_functions[] = {
    {"eject", l_media_eject},
    {"insert", l_media_insert},
    {"path", l_media_path},
    {NULL, NULL},
};

void luaengine_open_state(lua_State *L)
{
    luaL_newmetatable(L, SNAPSHOT_TYPE);
    luaL_setfuncs(L, snapshot_metamethods, 0);
    lua_pop(L, 1);
    luaL_newlib(L, state_functions);
    lua_setglobal(L, "state");
    luaL_newlib(L, media_functions);
    lua_setglobal(L, "media");
}

#endif  // WITH_LUA
