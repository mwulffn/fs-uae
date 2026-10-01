// Remote control of the Lua engine over a TCP socket.
//
// Each line received is a JSON object {"id": ..., "code": "<Lua code>"}. The
// code is run as a task, and when the task finishes one line is sent back:
// {"id": ..., "ok": true, "results": [...], "output": "<printed text>"} or
// {"id": ..., "ok": false, "error": "<message>", "output": "..."}.
// Lines with an "event" key instead of an "id" can be sent at any time.

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LUA

#include "luaengine.h"

#include "uae/socket.h"

#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

#define MAX_REQUEST_SIZE (16 * 1024 * 1024)

struct remote_client {
    int id;
    uae_socket socket;
    std::string input;
};

static uae_socket g_listen_socket = UAE_SOCKET_INVALID;
static std::vector<remote_client> g_clients;
static int g_next_client_id = 1;

static void close_client(remote_client &client)
{
    if (client.socket != UAE_SOCKET_INVALID) {
        write_log("[LUA] Client %d disconnected\n", client.id);
        uae_socket_close(client.socket);
        client.socket = UAE_SOCKET_INVALID;
    }
}

// Closed clients stay in the list (with an invalid socket) until this is
// called, so the list is not modified while it is being iterated.
static void remove_closed_clients(void)
{
    for (size_t i = g_clients.size(); i > 0; i--) {
        if (g_clients[i - 1].socket == UAE_SOCKET_INVALID) {
            g_clients.erase(g_clients.begin() + i - 1);
        }
    }
}

// Writing to a client which has gone away must fail, and not kill FS-UAE
// with SIGPIPE. Linux has a flag for that, macOS a socket option (see
// accept_client).
static int write_socket(uae_socket s, const char *data, int size)
{
#ifdef MSG_NOSIGNAL
    return (int) send(s, data, size, MSG_NOSIGNAL);
#else
    return uae_socket_write(s, data, size);
#endif
}

static void send_line(remote_client &client, std::string line)
{
    if (client.socket == UAE_SOCKET_INVALID) {
        return;
    }
    line += '\n';
    size_t sent = 0;
    while (sent < line.size()) {
        int n = write_socket(client.socket, line.data() + sent, (int) (line.size() - sent));
        if (n <= 0) {
            close_client(client);
            return;
        }
        sent += n;
    }
}

static remote_client *find_client(int id)
{
    for (remote_client &client : g_clients) {
        if (client.id == id) {
            return &client;
        }
    }
    return NULL;
}

static void send_error(remote_client &client, const std::string &id, const char *message)
{
    std::string line = "{\"id\":" + id + ",\"ok\":false,\"error\":";
    luaengine_json_append_string(line, message, strlen(message));
    line += ",\"output\":\"\"}";
    send_line(client, line);
}

void luaengine_remote_task_finished(luaengine_task *task, bool ok, int nresults)
{
    remote_client *client = find_client(task->client);
    if (client == NULL) {
        return;
    }
    lua_State *T = task->thread;
    std::string line = "{\"id\":" + task->request_id;
    if (ok) {
        line += ",\"ok\":true,\"results\":[";
        for (int i = 0; i < nresults; i++) {
            if (i > 0) {
                line += ',';
            }
            luaengine_json_append_value(line, T, lua_gettop(T) - nresults + 1 + i);
        }
        line += ']';
    } else {
        size_t len;
        const char *message = luaL_tolstring(T, -1, &len);
        line += ",\"ok\":false,\"error\":";
        luaengine_json_append_string(line, message, len);
        lua_pop(T, 1);
    }
    line += ",\"output\":";
    luaengine_json_append_string(line, task->output.data(), task->output.size());
    line += '}';
    send_line(*client, line);
}

void luaengine_remote_send_event(const char *event, const std::string &fields)
{
    std::string line = "{\"event\":";
    luaengine_json_append_string(line, event, strlen(event));
    if (!fields.empty()) {
        line += ',' + fields;
    }
    line += '}';
    for (remote_client &client : g_clients) {
        send_line(client, line);
    }
}

static void handle_request(remote_client &client, const std::string &request)
{
    lua_State *L = g_luaengine_state;
    std::string id, code, error;
    if (!luaengine_json_parse_request(request.data(), request.size(), id, code, error)) {
        send_error(client, id, error.c_str());
        return;
    }
    // Try the code as an expression first, so "1 + 1" returns a result.
    std::string expression = "return " + code;
    if (luaL_loadbuffer(L, expression.data(), expression.size(), "=remote") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=remote") != LUA_OK) {
            send_error(client, id, lua_tostring(L, -1));
            lua_pop(L, 1);
            return;
        }
    }
    luaengine_start_task(L, 0, client.id, id.c_str());
}

static void accept_client(void)
{
    uae_socket s = uae_socket_accept(g_listen_socket);
    if (s == UAE_SOCKET_INVALID) {
        return;
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    remote_client client;
    client.id = g_next_client_id++;
    client.socket = s;
    g_clients.push_back(client);
    write_log("[LUA] Client %d connected\n", client.id);
}

static bool read_client(size_t index)
{
    char buf[4096];
    bool activity = false;
    // Running a request can add clients, so g_clients[index] is looked up
    // again every time it is needed.
    while (g_clients[index].socket != UAE_SOCKET_INVALID &&
           (uae_socket_select_read(g_clients[index].socket) & UAE_SELECT_READ)) {
        int n = uae_socket_read(g_clients[index].socket, buf, sizeof buf);
        if (n <= 0) {
            close_client(g_clients[index]);
            break;
        }
        activity = true;
        g_clients[index].input.append(buf, n);
        size_t end;
        while ((end = g_clients[index].input.find('\n')) != std::string::npos) {
            std::string request = g_clients[index].input.substr(0, end);
            g_clients[index].input.erase(0, end + 1);
            if (!request.empty()) {
                handle_request(g_clients[index], request);
            }
        }
        if (g_clients[index].input.size() > MAX_REQUEST_SIZE) {
            close_client(g_clients[index]);
        }
    }
    return activity;
}

bool luaengine_remote_poll(void)
{
    if (g_listen_socket == UAE_SOCKET_INVALID) {
        return false;
    }
    bool activity = false;
    if (uae_socket_select_read(g_listen_socket) & UAE_SELECT_READ) {
        accept_client();
        activity = true;
    }
    for (size_t i = 0; i < g_clients.size(); i++) {
        if (read_client(i)) {
            activity = true;
        }
    }
    remove_closed_clients();
    return activity;
}

bool luaengine_remote_open(int port)
{
    char port_string[16];
    snprintf(port_string, sizeof port_string, "%d", port);
    // Lua code can do anything the emulator process can, so only local
    // connections are accepted.
    g_listen_socket = uae_tcp_listen("127.0.0.1", port_string, UAE_SOCKET_REUSEADDR);
    if (g_listen_socket == UAE_SOCKET_INVALID) {
        write_log("[LUA] Could not listen on port %d\n", port);
        return false;
    }
    write_log("[LUA] Listening on 127.0.0.1 port %d\n", port);
    return true;
}

void luaengine_remote_close(void)
{
    for (remote_client &client : g_clients) {
        close_client(client);
    }
    g_clients.clear();
    if (g_listen_socket != UAE_SOCKET_INVALID) {
        uae_socket_close(g_listen_socket);
        g_listen_socket = UAE_SOCKET_INVALID;
    }
}

#endif  // WITH_LUA
