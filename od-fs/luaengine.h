#ifndef UAE_LUAENGINE_H
#define UAE_LUAENGINE_H

#ifdef WITH_LUA

#include <lua.hpp>

#include <string>

#include "uae/types.h"

// The Lua state shared by startup scripts (lua=<file>), or NULL when Lua
// is not running. Only to be used from the emulation thread.
extern lua_State *g_luaengine_state;

// A task is a Lua coroutine which can wait for emulated frames to pass.
struct luaengine_task {
    lua_State *thread;
    // Registry reference keeping the thread alive.
    int ref;
    int64_t wake_frame;
    // The task is resumed when the emulation stops (or at wake_frame).
    bool wait_stop;
    // The remote client and request which started the task, or -1.
    int client;
    std::string request_id;
    // Text printed by a task started by a remote client.
    std::string output;
};

// Runs the function on the stack, with nargs arguments above it, as a new
// task. The function and its arguments are popped. If client is not -1, the
// result is sent to that remote client as the reply to request_id.
void luaengine_start_task(lua_State *L, int nargs, int client = -1, const char *request_id = "");

// Yields the calling task until the given number of frames have been
// emulated, and then continues in k (if not NULL), which is given context.
// If the emulation is paused, it runs for these frames only.
int luaengine_yield_frames(
    lua_State *L, lua_Integer frames, lua_KFunction k = NULL, lua_KContext context = 0);

// Stops the emulation before the next instruction, as emu.pause does. The
// reason, and the id and address when given, are what dbg.wait returns.
void luaengine_stop(const char *reason, int id = 0, uaecptr address = 0);
void luaengine_resume(void);
bool luaengine_stop_requested(void);
// Pushes a table describing why the emulation is stopped, or false.
void luaengine_push_stop_info(lua_State *L);
// Yields the calling task until the emulation stops, and returns the stop
// information to it. If max_frames is not 0, the task also continues (with
// no values returned) when that many frames have been emulated.
int luaengine_yield_until_stopped(lua_State *L, lua_Integer max_frames);

// Logs the error message on top of the stack and pops it.
void luaengine_log_error(lua_State *L, const char *context);

// Functions creating the global tables with the Lua API.

void luaengine_open_cpu(lua_State *L);
void luaengine_open_dbg(lua_State *L);
void luaengine_open_input(lua_State *L);
void luaengine_open_mem(lua_State *L);
void luaengine_open_state(lua_State *L);
void luaengine_open_video(lua_State *L);

// luaengine-debug.cpp

// True while uae_lua_service must be called before every instruction.
bool luaengine_debug_active(void);
// Checks breakpoints and counts instruction steps. Called before every
// instruction while luaengine_debug_active.
void luaengine_debug_instruction(void);
// Makes the next call to luaengine_debug_instruction ignore the current
// instruction, which has already been checked (or is where we stopped).
void luaengine_debug_mark_instruction(void);
// Memory accesses made while suspended do not run tap callbacks.
void luaengine_suspend_taps(bool suspend);
void luaengine_debug_free(void);

// luaengine-json.cpp

void luaengine_json_append_string(std::string &out, const char *s, size_t len);
void luaengine_json_append_value(std::string &out, lua_State *L, int index);
// Parses {"id": ..., "code": "..."}. The id is returned as JSON text.
bool luaengine_json_parse_request(
    const char *text, size_t len, std::string &id, std::string &code, std::string &error);

// luaengine-remote.cpp

bool luaengine_remote_open(int port);
void luaengine_remote_close(void);
// Handles new connections and requests. Returns false if there were none.
bool luaengine_remote_poll(void);
void luaengine_remote_task_finished(luaengine_task *task, bool ok, int nresults);
// Sends {"event": <event>, <fields>} to all clients. The fields are JSON
// text ("key": value, ...) and can be empty.
void luaengine_remote_send_event(const char *event, const std::string &fields);

#endif  // WITH_LUA

#endif  // UAE_LUAENGINE_H
