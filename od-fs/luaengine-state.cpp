// Lua functions for save states (the state table) and floppy disks (the
// media table).

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "disk.h"
#include "options.h"
#include "savestate.h"
#include "zfile.h"

// While a directory hard drive is handling a request from the Amiga, the
// UAE core does not save states, and tries again at the end of the next
// frame. This is how many frames state.save and state.snapshot wait.
#define MAX_SAVE_FRAMES 100

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
    if (savestate_state != 0) {
        // The state is saved when savestate_state is cleared.
        savestate_state = 0;
        return luaL_error(L, "could not save the state");
    }
    return 0;
}

// state.save(path) saves a state file. The UAE core saves states at the
// end of a frame, so this returns when the current frame is finished.
static int l_state_save(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_argcheck(L, strlen(path) < MAX_DPATH, 1, "path is too long");
    _tcscpy(savestate_fname, path);
    savestate_state = STATE_SAVE;
    return luaengine_yield_frames(L, 1, save_finished, 1);
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

// state.snapshot() saves a state in memory and returns it. Like state.save,
// it returns when the current frame is finished.
static int l_state_snapshot(lua_State *L)
{
    savestate_memory_save_request();
    return luaengine_yield_frames(L, 1, snapshot_finished, 1);
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
