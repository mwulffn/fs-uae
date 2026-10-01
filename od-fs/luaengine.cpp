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
#include "newcpu.h"
#include "options.h"

#include <vector>

lua_State *g_luaengine_state;

static std::vector<luaengine_task *> g_tasks;
// Registry references to the functions registered with emu.on_frame.
static std::vector<int> g_frame_callbacks;
static int64_t g_frame;
// Number of vsyncs seen since the last call to uae_lua_service.
static int g_pending_frames;
// The task being resumed, or NULL.
static luaengine_task *g_current_task;

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
    size_t len;
    const char *text = lua_tolstring(L, -1, &len);
    if (g_current_task != NULL && g_current_task->client != -1) {
        g_current_task->output.append(text, len);
        g_current_task->output += '\n';
    } else {
        write_log("[LUA] %s\n", text);
        std::string fields = "\"text\":";
        luaengine_json_append_string(fields, text, len);
        luaengine_remote_send_event("print", fields);
    }
    return 0;
}

// Resumes the task with nargs arguments on its stack. The task is freed if
// it finished, or put on the waiting list if it yielded.
static void resume_task(luaengine_task *task, int nargs)
{
    lua_State *L = g_luaengine_state;
    int nresults;
    luaengine_task *previous_task = g_current_task;
    g_current_task = task;
    int status = lua_resume(task->thread, L, nargs, &nresults);
    g_current_task = previous_task;
    if (status == LUA_YIELD) {
        // The yielded value is the number of frames to wait.
        lua_Integer frames = nresults > 0 ? lua_tointeger(task->thread, -1) : 1;
        lua_pop(task->thread, nresults);
        task->wake_frame = g_frame + (frames > 1 ? frames : 1);
        g_tasks.push_back(task);
        return;
    }
    if (task->client != -1) {
        luaengine_remote_task_finished(task, status == LUA_OK, nresults);
    } else if (status != LUA_OK) {
        luaL_traceback(L, task->thread, lua_tostring(task->thread, -1), 0);
        luaengine_log_error(L, "Error");
    }
    luaL_unref(L, LUA_REGISTRYINDEX, task->ref);
    delete task;
}

void luaengine_start_task(lua_State *L, int nargs, int client, const char *request_id)
{
    luaengine_task *task = new luaengine_task();
    task->thread = lua_newthread(L);
    task->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    task->wake_frame = 0;
    task->client = client;
    task->request_id = request_id;
    lua_xmove(L, task->thread, nargs + 1);
    resume_task(task, nargs);
}

static void resume_due_tasks(void)
{
    // Tasks resumed here may be added to g_tasks again.
    std::vector<luaengine_task *> tasks;
    tasks.swap(g_tasks);
    for (luaengine_task *task : tasks) {
        if (task->wake_frame <= g_frame) {
            resume_task(task, 0);
        } else {
            g_tasks.push_back(task);
        }
    }
}

static void run_frame_callbacks(void)
{
    lua_State *L = g_luaengine_state;
    // Callbacks may register or remove callbacks.
    std::vector<int> callbacks = g_frame_callbacks;
    for (int ref : callbacks) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            luaengine_log_error(L, "Error in frame callback");
        }
    }
    // The original Lua layer called this global function on every vsync.
    if (lua_getglobal(L, "on_uae_vsync") == LUA_TFUNCTION) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            luaengine_log_error(L, "on_uae_vsync");
        }
    } else {
        lua_pop(L, 1);
    }
}

static int l_emu_log(lua_State *L)
{
    write_log("[LUA] %s\n", luaL_checkstring(L, 1));
    return 0;
}

static int l_emu_frame(lua_State *L)
{
    lua_pushinteger(L, g_frame);
    return 1;
}

static int l_emu_wait_frames(lua_State *L)
{
    lua_pushinteger(L, luaL_optinteger(L, 1, 1));
    return lua_yield(L, 1);
}

static int l_emu_wait_next_frame(lua_State *L)
{
    lua_pushinteger(L, 1);
    return lua_yield(L, 1);
}

static int l_emu_on_frame(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_frame_callbacks.push_back(ref);
    lua_pushinteger(L, ref);
    return 1;
}

static int l_emu_remove_frame_callback(lua_State *L)
{
    int ref = (int) luaL_checkinteger(L, 1);
    for (size_t i = 0; i < g_frame_callbacks.size(); i++) {
        if (g_frame_callbacks[i] == ref) {
            g_frame_callbacks.erase(g_frame_callbacks.begin() + i);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            break;
        }
    }
    return 0;
}

static const luaL_Reg emu_functions[] = {
    {"frame", l_emu_frame},
    {"log", l_emu_log},
    {"on_frame", l_emu_on_frame},
    {"remove_frame_callback", l_emu_remove_frame_callback},
    {"wait_frames", l_emu_wait_frames},
    {"wait_next_frame", l_emu_wait_next_frame},
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
    luaengine_start_task(L, 0);
}

void uae_lua_loadall(void)
{
    bool have_scripts = false;
    for (int i = 0; i < MAX_LUA_STATES; i++) {
        if (currprefs.luafiles[i][0]) {
            have_scripts = true;
        }
    }
    if (!have_scripts && currprefs.lua_port == 0) {
        return;
    }
    // The emulation is started again when a new configuration is loaded.
    uae_lua_free();
    g_luaengine_state = luaL_newstate();
    uae_lua_init_state(g_luaengine_state);
    if (currprefs.lua_port != 0) {
        luaengine_remote_open(currprefs.lua_port);
    }
    for (int i = 0; i < MAX_LUA_STATES; i++) {
        uae_lua_load(currprefs.luafiles[i]);
    }
}

void uae_lua_run_handler(const char *name)
{
    if (g_luaengine_state == NULL) {
        return;
    }
    // This is called from the middle of an emulated instruction (at least
    // in cycle-exact modes), so the work is left to uae_lua_service, which
    // the CPU loop calls before the next instruction.
    if (strcmp(name, "on_uae_vsync") == 0) {
        g_pending_frames += 1;
        set_special(SPCFLAG_BRK);
    }
}

void uae_lua_service(void)
{
    if (g_luaengine_state == NULL) {
        return;
    }
    // This function is also called when the debugger is active, possibly
    // after every instruction, so the socket is only checked once per frame.
    bool new_frame = g_pending_frames > 0;
    while (g_pending_frames > 0) {
        g_pending_frames -= 1;
        g_frame += 1;
        run_frame_callbacks();
        resume_due_tasks();
    }
    if (new_frame) {
        luaengine_remote_poll();
    }
}

void uae_lua_init(void)
{
}

void uae_lua_free(void)
{
    luaengine_remote_close();
    for (luaengine_task *task : g_tasks) {
        delete task;
    }
    g_tasks.clear();
    g_frame_callbacks.clear();
    g_pending_frames = 0;
    if (g_luaengine_state != NULL) {
        lua_close(g_luaengine_state);
        g_luaengine_state = NULL;
    }
}

#endif  // WITH_LUA
