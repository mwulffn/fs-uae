// Lua functions for keyboard, joystick and mouse input (the input table).

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "inputdevice.h"
#include "options.h"
#include "uae/fs.h"

struct named_event {
    const char *name;
    int event;
};

// All the keys in inputevents.def (KEY_A, KEY_F1, ...), without the KEY_
// at the start of the names.
#define DEFEVENT(A, B, C, D, E, F)
#define DEFEVENT2(A, B, B2, C, D, E, F, G)
#define DEFEVENTKB(A, B, C, F, PC) {#A + 4, INPUTEVENT_##A},
static const named_event key_events[] = {
#include "../inputevents.def"
    {NULL, 0},
};
#undef DEFEVENT
#undef DEFEVENT2
#undef DEFEVENTKB

// The events for port 1. The ones for port 0 (INPUTEVENT_JOY1_...) are
// found by subtracting the distance between the two groups.
static const named_event joystick_events[] = {
    {"left", INPUTEVENT_JOY2_LEFT},
    {"right", INPUTEVENT_JOY2_RIGHT},
    {"up", INPUTEVENT_JOY2_UP},
    {"down", INPUTEVENT_JOY2_DOWN},
    {"fire", INPUTEVENT_JOY2_FIRE_BUTTON},
    {"fire2", INPUTEVENT_JOY2_2ND_BUTTON},
    {"fire3", INPUTEVENT_JOY2_3RD_BUTTON},
    {NULL, 0},
};

static const named_event port_modes[] = {
    {"none", AMIGA_JOYPORT_NONE},
    {"mouse", AMIGA_JOYPORT_MOUSE},
    {"joystick", AMIGA_JOYPORT_DJOY},
    {"cd32", AMIGA_JOYPORT_CD32JOY},
    {NULL, 0},
};

static int check_port(lua_State *L, int arg)
{
    lua_Integer port = luaL_checkinteger(L, arg);
    luaL_argcheck(L, port == 0 || port == 1, arg, "port must be 0 or 1");
    return (int) port;
}

static int check_named_event(lua_State *L, int arg, const named_event *events, const char *what)
{
    const char *name = luaL_checkstring(L, arg);
    for (int i = 0; events[i].name; i++) {
        if (strcasecmp(events[i].name, name) == 0) {
            return events[i].event;
        }
    }
    return luaL_error(L, "unknown %s '%s'", what, name);
}

// input.key(name, down) presses or releases a key. The names are the ones
// in inputevents.def without KEY_, for example "a", "f1" and "return".
static int l_input_key(lua_State *L)
{
    int event = check_named_event(L, 1, key_events, "key");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    amiga_send_input_event(event, lua_toboolean(L, 2));
    return 0;
}

// input.joy(port, button, down). Port 1 is the normal joystick port. The
// buttons are "left", "right", "up", "down", "fire", "fire2" and "fire3".
static int l_input_joy(lua_State *L)
{
    int port = check_port(L, 1);
    int event = check_named_event(L, 2, joystick_events, "joystick button");
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    if (port == 0) {
        event -= INPUTEVENT_JOY2_LEFT - INPUTEVENT_JOY1_LEFT;
    }
    amiga_send_input_event(event, lua_toboolean(L, 3));
    return 0;
}

// input.mouse(dx, dy) moves the mouse in port 0.
static int l_input_mouse(lua_State *L)
{
    int dx = (int) luaL_checkinteger(L, 1);
    int dy = (int) luaL_checkinteger(L, 2);
    amiga_send_input_event(INPUTEVENT_MOUSE1_HORIZ, dx);
    amiga_send_input_event(INPUTEVENT_MOUSE1_VERT, dy);
    return 0;
}

// input.mouse_button(button, down), where button is 1 (left), 2 (right) or
// 3 (middle).
static int l_input_mouse_button(lua_State *L)
{
    static const int events[] = {
        INPUTEVENT_JOY1_FIRE_BUTTON, INPUTEVENT_JOY1_2ND_BUTTON, INPUTEVENT_JOY1_3RD_BUTTON};
    lua_Integer button = luaL_checkinteger(L, 1);
    luaL_argcheck(L, button >= 1 && button <= 3, 1, "button must be 1, 2 or 3");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    amiga_send_input_event(events[button - 1], lua_toboolean(L, 2));
    return 0;
}

// input.port_mode(port, mode) selects what is connected to a port: "none",
// "mouse", "joystick" or "cd32".
static int l_input_port_mode(lua_State *L)
{
    int port = check_port(L, 1);
    int mode = check_named_event(L, 2, port_modes, "port mode");
    amiga_set_joystick_port_mode(port, mode);
    return 0;
}

static const luaL_Reg input_functions[] = {
    {"joy", l_input_joy},
    {"key", l_input_key},
    {"mouse", l_input_mouse},
    {"mouse_button", l_input_mouse_button},
    {"port_mode", l_input_port_mode},
    {NULL, NULL},
};

// input.type(text, frames) types the text on a US keyboard, holding each
// key for the given number of frames (default 2).
static const char *input_type_source = R"LUA(
local plain = {
    [" "] = "space", ["\n"] = "return", ["\t"] = "tab", ["-"] = "sub",
    ["="] = "equals", ["\\"] = "backslash", ["["] = "leftbracket",
    ["]"] = "rightbracket", [";"] = "semicolon", ["'"] = "singlequote",
    [","] = "comma", ["."] = "period", ["/"] = "div", ["`"] = "backquote",
}
local shifted = {
    ["!"] = "1", ["@"] = "2", ["#"] = "3", ["$"] = "4", ["%"] = "5",
    ["^"] = "6", ["&"] = "7", ["*"] = "8", ["("] = "9", [")"] = "0",
    ["_"] = "sub", ["+"] = "equals", ["|"] = "backslash",
    ["{"] = "leftbracket", ["}"] = "rightbracket", [":"] = "semicolon",
    ['"'] = "singlequote", ["<"] = "comma", [">"] = "period", ["?"] = "div",
    ["~"] = "backquote",
}

function input.type(text, frames)
    frames = frames or 2
    for c in text:gmatch(".") do
        local key, shift = plain[c], false
        if not key then
            if shifted[c] then
                key, shift = shifted[c], true
            elseif c:match("%u") then
                key, shift = c:lower(), true
            elseif c:match("[%l%d]") then
                key = c
            else
                error("cannot type the character '" .. c .. "'")
            end
        end
        if shift then input.key("shift_left", true) end
        input.key(key, true)
        emu.wait_frames(frames)
        input.key(key, false)
        if shift then input.key("shift_left", false) end
        emu.wait_frames(frames)
    end
end
)LUA";

void luaengine_open_input(lua_State *L)
{
    luaL_newlib(L, input_functions);
    lua_setglobal(L, "input");
    if (luaL_dostring(L, input_type_source) != LUA_OK) {
        luaengine_log_error(L, "input.type");
    }
}

#endif  // WITH_LUA
